#include <unordered_set>

#include <libslic3r/Exception.hpp>
#include <libslic3r/SLAPrintSteps.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

// Need the cylinder method for the the drainholes in hollowing step
#include <libslic3r/SLA/SupportTreeBuilder.hpp>

#include <libslic3r/Execution/ExecutionTBB.hpp>
#include <libslic3r/SLA/Pad.hpp>
#include <libslic3r/SLA/SupportPointGenerator.hpp>

#include <libslic3r/ElephantFootCompensation.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/QuadricEdgeCollapse.hpp>

#include <boost/log/trivial.hpp>

#include "I18N.hpp"

#include <libnest2d/tools/benchmark.h>

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {

const std::array<unsigned, slaposCount> OBJ_STEP_LEVELS = {
    10, // slaposHollowing,
    10, // slaposDrillHoles
    10, // slaposObjectSlice,
    20, // slaposSupportPoints,
    10, // slaposSupportTree,
    10, // slaposPad,
    30, // slaposSliceSupports,
};

std::string OBJ_STEP_LABELS(size_t idx)
{
    switch (idx) {
    case slaposHollowing:            return L("Hollowing model");
    case slaposDrillHoles:           return L("Drilling holes into model.");
    case slaposObjectSlice:          return L("Slicing model");
    case slaposSupportPoints:        return L("Generating support points");
    case slaposSupportTree:          return L("Generating support tree");
    case slaposPad:                  return L("Generating pad");
    case slaposSliceSupports:        return L("Slicing supports");
    default:;
    }
    assert(false);
    return "Out of bounds!";
}

const std::array<unsigned, slapsCount> PRINT_STEP_LEVELS = {
    10, // slapsMergeSlicesAndEval
    90, // slapsRasterize
};

std::string PRINT_STEP_LABELS(size_t idx)
{
    switch (idx) {
    case slapsMergeSlicesAndEval:   return L("Merging slices and calculating statistics");
    case slapsRasterize:            return L("Rasterizing layers");
    default:;
    }
    assert(false); return "Out of bounds!";
}

}

SLAPrint::Steps::Steps(SLAPrint *print)
    : m_print{print}
    , m_rng{std::random_device{}()}
    , objcount{m_print->m_objects.size()}
    , ilhd{m_print->m_material_config.initial_layer_height.getFloat()}
    , ilh{float(ilhd)}
    , ilhs{scaled(ilhd)}
    , objectstep_scale{(max_objstatus - min_objstatus) / (objcount * 100.0)}
{}

void SLAPrint::Steps::apply_printer_corrections(SLAPrintObject &po, SliceOrigin o)
{
    if (o == soSupport && !po.m_supportdata) return;

    auto faded_lyrs = size_t(po.m_config.faded_layers.getInt());
    double min_w = m_print->m_printer_config.elefant_foot_min_width.getFloat() / 2.;
    double start_efc = m_print->m_printer_config.elefant_foot_compensation.getFloat();

    double doffs = m_print->m_printer_config.absolute_correction.getFloat();
    coord_t clpr_offs = scaled(doffs);

    faded_lyrs = std::min(po.m_slice_index.size(), faded_lyrs);
    size_t faded_lyrs_efc = std::max(size_t(1), faded_lyrs - 1);

    auto efc = [start_efc, faded_lyrs_efc](size_t pos) {
        return (faded_lyrs_efc - pos) * start_efc / faded_lyrs_efc;
    };

    std::vector<ExPolygons> &slices = o == soModel ?
                                          po.m_model_slices :
                                          po.m_supportdata->support_slices;

    if (clpr_offs != 0) for (size_t i = 0; i < po.m_slice_index.size(); ++i) {
        size_t idx = po.m_slice_index[i].get_slice_idx(o);
        if (idx < slices.size())
            slices[idx] = offset_ex(slices[idx], float(clpr_offs));
    }

    if (start_efc > 0.) for (size_t i = 0; i < faded_lyrs; ++i) {
        size_t idx = po.m_slice_index[i].get_slice_idx(o);
        if (idx < slices.size())
            slices[idx] = elephant_foot_compensation(slices[idx], min_w, efc(i));
    }
}

void SLAPrint::Steps::hollow_model(SLAPrintObject &po)
{
    po.m_hollowing_data.reset();

    if (! po.m_config.hollowing_enable.getBool()) {
        BOOST_LOG_TRIVIAL(info) << "Skipping hollowing step!";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Performing hollowing step!";

    double thickness = po.m_config.hollowing_min_thickness.getFloat();
    double quality  = po.m_config.hollowing_quality.getFloat();
    double closing_d = po.m_config.hollowing_closing_distance.getFloat();
    sla::HollowingConfig hlwcfg{thickness, quality, closing_d};
    sla::JobController ctl;
    ctl.stopcondition = [this]() { return canceled(); };
    ctl.cancelfn = [this]() { throw_if_canceled(); };

    sla::InteriorPtr interior = generate_interior(po.transformed_mesh().its, hlwcfg, ctl);

    if (!interior || sla::get_mesh(*interior).empty())
        BOOST_LOG_TRIVIAL(warning) << "Hollowed interior is empty!";
    else {
        po.m_hollowing_data.reset(new SLAPrintObject::HollowingData());
        po.m_hollowing_data->interior = std::move(interior);

        indexed_triangle_set &m = sla::get_mesh(*po.m_hollowing_data->interior);

        if (!m.empty()) {
            // simplify mesh lossless
            float loss_less_max_error = 2*std::numeric_limits<float>::epsilon();
            its_quadric_edge_collapse(m, 0U, &loss_less_max_error);

            its_compactify_vertices(m);
            its_merge_vertices(m);

            // flip normals back...
            sla::swap_normals(m);
        }
    }
}

static indexed_triangle_set
remove_unconnected_vertices(const indexed_triangle_set &its)
{
    if (its.indices.empty()) {};

    indexed_triangle_set M;
    its_compactify_vertices(M);

    return M;
}

// Drill holes into the hollowed/original mesh.
void SLAPrint::Steps::drill_holes(SLAPrintObject &po)
{
    bool needs_drilling = ! po.m_model_object->sla_drain_holes.empty();
    bool is_hollowed =
        (po.m_hollowing_data && po.m_hollowing_data->interior &&
         !sla::get_mesh(*po.m_hollowing_data->interior).empty());

    if (! is_hollowed && ! needs_drilling) {
        // In this case we can dump any data that might have been
        // generated on previous runs.
        po.m_hollowing_data.reset();
        return;
    }

    if (! po.m_hollowing_data)
        po.m_hollowing_data.reset(new SLAPrintObject::HollowingData());

    // Hollowing and/or drilling is active, m_hollowing_data is valid.

    // Regenerate hollowed mesh, even if it was there already. It may contain
    // holes that are no longer on the frontend.
    TriangleMesh &hollowed_mesh = po.m_hollowing_data->hollow_mesh_with_holes;
    hollowed_mesh = po.transformed_mesh();
    if (is_hollowed)
        sla::hollow_mesh(hollowed_mesh, *po.m_hollowing_data->interior);

    TriangleMesh &mesh_view = po.m_hollowing_data->hollow_mesh_with_holes_trimmed;

    if (! needs_drilling) {
        mesh_view = po.transformed_mesh();

        if (is_hollowed)
            sla::hollow_mesh(mesh_view, *po.m_hollowing_data->interior,
                             sla::hfRemoveInsideTriangles);

        BOOST_LOG_TRIVIAL(info) << "Drilling skipped (no holes).";
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "Drilling drainage holes.";
    sla::DrainHoles drainholes = po.transformed_drainhole_points();

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
        hollowed_mesh.its.vertices,
        hollowed_mesh.its.indices
    );

    std::uniform_real_distribution<float> dist(0., float(EPSILON));
    auto holes_mesh_cgal = MeshBoolean::cgal::triangle_mesh_to_cgal({}, {});
    indexed_triangle_set part_to_drill = hollowed_mesh.its;

    bool hole_fail = false;
    for (size_t i = 0; i < drainholes.size(); ++i) {
        sla::DrainHole holept = drainholes[i];

        holept.normal += Vec3f{dist(m_rng), dist(m_rng), dist(m_rng)};
        holept.normal.normalize();
        holept.pos += Vec3f{dist(m_rng), dist(m_rng), dist(m_rng)};
        indexed_triangle_set m = holept.to_mesh();

        part_to_drill.indices.clear();
        auto bb = bounding_box(m);
        Eigen::AlignedBox<float, 3> ebb{bb.min.cast<float>(),
                                        bb.max.cast<float>()};

        AABBTreeIndirect::traverse(
                    tree,
                    AABBTreeIndirect::intersecting(ebb),
                    [&part_to_drill, &hollowed_mesh](const auto& node)
        {
            part_to_drill.indices.emplace_back(hollowed_mesh.its.indices[node.idx]);
        });

        auto cgal_meshpart = MeshBoolean::cgal::triangle_mesh_to_cgal(
            remove_unconnected_vertices(part_to_drill));

        if (MeshBoolean::cgal::does_self_intersect(*cgal_meshpart)) {
            BOOST_LOG_TRIVIAL(error) << "Failed to drill hole";

            hole_fail = drainholes[i].failed =
                    po.model_object()->sla_drain_holes[i].failed = true;

            continue;
        }

        auto cgal_hole = MeshBoolean::cgal::triangle_mesh_to_cgal(m);
        MeshBoolean::cgal::plus(*holes_mesh_cgal, *cgal_hole);
    }

    if (MeshBoolean::cgal::does_self_intersect(*holes_mesh_cgal))
        throw Slic3r::SlicingError(L("Too many overlapping holes."));

    auto hollowed_mesh_cgal = MeshBoolean::cgal::triangle_mesh_to_cgal(hollowed_mesh);

    if (!MeshBoolean::cgal::does_bound_a_volume(*hollowed_mesh_cgal)) {
        po.active_step_add_warning(
            PrintStateBase::WarningLevel::NON_CRITICAL,
            L("Mesh to be hollowed is not suitable for hollowing (does not "
              "bound a volume)."));
    }

    if (!MeshBoolean::cgal::empty(*holes_mesh_cgal)
        && !MeshBoolean::cgal::does_bound_a_volume(*holes_mesh_cgal)) {
        po.active_step_add_warning(
            PrintStateBase::WarningLevel::NON_CRITICAL,
            L("Unable to drill the current configuration of holes into the "
              "model."));
    }

    try {
        if (!MeshBoolean::cgal::empty(*holes_mesh_cgal))
            MeshBoolean::cgal::minus(*hollowed_mesh_cgal, *holes_mesh_cgal);

        hollowed_mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*hollowed_mesh_cgal);
        mesh_view = hollowed_mesh;

        if (is_hollowed) {
            auto &interior = *po.m_hollowing_data->interior;
            std::vector<bool> exclude_mask =
                    create_exclude_mask(mesh_view.its, interior, drainholes);

            sla::remove_inside_triangles(mesh_view, interior, exclude_mask);
        }
    } catch (const Slic3r::RuntimeError &) {
        throw Slic3r::SlicingError(L(
            "Drilling holes into the mesh failed. "
            "This is usually caused by broken model. Try to fix it first."));
    }

    if (hole_fail)
        po.active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
                                   L("Failed to drill some holes into the model"));
}

// The slicing will be performed on an imaginary 1D grid which starts from
// the bottom of the bounding box created around the supported model. So
// the first layer which is usually thicker will be part of the supports
// not the model geometry. Exception is when the model is not in the air
// (elevation is zero) and no pad creation was requested. In this case the
// model geometry starts on the ground level and the initial layer is part
// of it. In any case, the model and the supports have to be sliced in the
// same imaginary grid (the height vector argument to TriangleMeshSlicer).
void SLAPrint::Steps::slice_model(SLAPrintObject &po)
{
    const TriangleMesh &mesh = po.get_mesh_to_slice();

    // We need to prepare the slice index...

    double  lhd  = m_print->m_objects.front()->m_config.layer_height.getFloat();
    float   lh   = float(lhd);
    coord_t lhs  = scaled(lhd);
    auto && bb3d = mesh.bounding_box();
    double  minZ = bb3d.min(Z) - po.get_elevation();
    double  maxZ = bb3d.max(Z);
    auto    minZf = float(minZ);
    coord_t minZs = scaled(minZ);
    coord_t maxZs = scaled(maxZ);

    po.m_slice_index.clear();

    size_t cap = size_t(1 + (maxZs - minZs - ilhs) / lhs);
    po.m_slice_index.reserve(cap);

    po.m_slice_index.emplace_back(minZs + ilhs, minZf + ilh / 2.f, ilh);

    for(coord_t h = minZs + ilhs + lhs; h <= maxZs; h += lhs)
        po.m_slice_index.emplace_back(h, unscaled<float>(h) - lh / 2.f, lh);

    // Just get the first record that is from the model:
    auto slindex_it =
        po.closest_slice_record(po.m_slice_index, float(bb3d.min(Z)));

    if(slindex_it == po.m_slice_index.end())
        //TRN To be shown at the status bar on SLA slicing error.
        throw Slic3r::RuntimeError(
            L("Slicing had to be stopped due to an internal error: "
              "Inconsistent slice index."));

    po.m_model_height_levels.clear();
    po.m_model_height_levels.reserve(po.m_slice_index.size());
    for(auto it = slindex_it; it != po.m_slice_index.end(); ++it)
        po.m_model_height_levels.emplace_back(it->slice_level());

    po.m_model_slices.clear();
    MeshSlicingParamsEx params;
    params.closing_radius = float(po.config().slice_closing_radius.value);
    switch (po.config().slicing_mode.value) {
    case SlicingMode::Regular:    params.mode = MeshSlicingParams::SlicingMode::Regular; break;
    case SlicingMode::EvenOdd:    params.mode = MeshSlicingParams::SlicingMode::EvenOdd; break;
    case SlicingMode::CloseHoles: params.mode = MeshSlicingParams::SlicingMode::Positive; break;
    }
    auto  thr        = [this]() { m_print->throw_if_canceled(); };
    auto &slice_grid = po.m_model_height_levels;
    po.m_model_slices = slice_mesh_ex(mesh.its, slice_grid, params, thr);

    sla::Interior *interior = po.m_hollowing_data ?
                                  po.m_hollowing_data->interior.get() :
                                  nullptr;

    if (interior && ! sla::get_mesh(*interior).empty()) {
        indexed_triangle_set interiormesh = sla::get_mesh(*interior);
        sla::swap_normals(interiormesh);
        params.mode = MeshSlicingParams::SlicingMode::Regular;

        std::vector<ExPolygons> interior_slices = slice_mesh_ex(interiormesh, slice_grid, params, thr);

        execution::for_each(
            ex_tbb, size_t(0), interior_slices.size(),
            [&po, &interior_slices](size_t i) {
                const ExPolygons &slice = interior_slices[i];
                po.m_model_slices[i] = diff_ex(po.m_model_slices[i], slice);
            },
            execution::max_concurrency(ex_tbb));
    }

    auto mit = slindex_it;
    for (size_t id = 0;
         id < po.m_model_slices.size() && mit != po.m_slice_index.end();
         id++) {
        mit->set_model_slice_idx(po, id); ++mit;
    }

    // We apply the printer correction offset here.
    apply_printer_corrections(po, soModel);

    if(po.m_config.supports_enable.getBool() || po.m_config.pad_enable.getBool())
    {
        po.m_supportdata.reset(new SLAPrintObject::SupportData(po.get_mesh_to_print()));
    }
}

// In this step we check the slices, identify island and cover them with
// support points. Then we sprinkle the rest of the mesh.
void SLAPrint::Steps::support_points(SLAPrintObject &po)
{
    // If supports are disabled, we can skip the model scan.
    if(!po.m_config.supports_enable.getBool()) return;

    if (!po.m_supportdata)
        po.m_supportdata.reset(new SLAPrintObject::SupportData(po.get_mesh_to_print()));

    po.m_supportdata->input.zoffset = bounding_box(po.get_mesh_to_print())
                                          .min.z();

    const ModelObject& mo = *po.m_model_object;

    BOOST_LOG_TRIVIAL(debug) << "Support point count "
                             << mo.sla_support_points.size();

    // Unless the user modified the points or we already did the calculation,
    // we will do the autoplacement. Otherwise we will just blindly copy the
    // frontend data into the backend cache.
    if (mo.sla_points_status != sla::PointsStatus::UserModified) {

        // calculate heights of slices (slices are calculated already)
        const std::vector<float>& heights = po.m_model_height_levels;

        // Tell the mesh where drain holes are. Although the points are
        // calculated on slices, the algorithm then raycasts the points
        // so they actually lie on the mesh.
//        po.m_supportdata->emesh.load_holes(po.transformed_drainhole_points());

        throw_if_canceled();
        sla::SupportPointGenerator::Config config;
        const SLAPrintObjectConfig& cfg = po.config();

        // the density config value is in percents:
        config.density_relative = float(cfg.support_points_density_relative / 100.f);
        config.minimal_distance = float(cfg.support_points_minimal_distance);
        config.head_diameter    = float(cfg.support_head_front_diameter);

        // scaling for the sub operations
        double d = objectstep_scale * OBJ_STEP_LEVELS[slaposSupportPoints] / 100.0;
        double init = current_status();

        auto statuscb = [this, d, init](unsigned st)
        {
            double current = init + st * d;
            if(std::round(current_status()) < std::round(current))
                report_status(current, OBJ_STEP_LABELS(slaposSupportPoints));
        };

        // Construction of this object does the calculation.
        throw_if_canceled();
        sla::SupportPointGenerator auto_supports(
            po.m_supportdata->input.emesh, po.get_model_slices(),
            heights, config, [this]() { throw_if_canceled(); }, statuscb);

        // Now let's extract the result.
        const std::vector<sla::SupportPoint>& points = auto_supports.output();
        throw_if_canceled();
        po.m_supportdata->input.pts = points;

        BOOST_LOG_TRIVIAL(debug)
            << "Automatic support points: "
            << po.m_supportdata->input.pts.size();

        // Using RELOAD_SLA_SUPPORT_POINTS to tell the Plater to pass
        // the update status to GLGizmoSlaSupports
        report_status(-1, L("Generating support points"),
                      SlicingStatus::RELOAD_SLA_SUPPORT_POINTS);
    } else {
        // There are either some points on the front-end, or the user
        // removed them on purpose. No calculation will be done.
        po.m_supportdata->input.pts = po.transformed_support_points();
    }
}

void SLAPrint::Steps::support_tree(SLAPrintObject &po)
{
    if(!po.m_supportdata) return;

//    sla::PadConfig pcfg = make_pad_cfg(po.m_config);

//    if (pcfg.embed_object)
//        po.m_supportdata->emesh.ground_level_offset(pcfg.wall_thickness_mm);

    // If the zero elevation mode is engaged, we have to filter out all the
    // points that are on the bottom of the object
    if (is_zero_elevation(po.config())) {
        remove_bottom_points(po.m_supportdata->input.pts,
                             float(
                                 po.m_supportdata->input.zoffset +
                                 EPSILON));
    }

    po.m_supportdata->input.cfg = make_support_cfg(po.m_config);
    po.m_supportdata->input.pad_cfg = make_pad_cfg(po.m_config);
//    po.m_supportdata->emesh.load_holes(po.transformed_drainhole_points());

    // scaling for the sub operations
    double d = objectstep_scale * OBJ_STEP_LEVELS[slaposSupportTree] / 100.0;
    double init = current_status();
    sla::JobController ctl;

    ctl.statuscb = [this, d, init](unsigned st, const std::string &logmsg) {
        double current = init + st * d;
        if (std::round(current_status()) < std::round(current))
            report_status(current, OBJ_STEP_LABELS(slaposSupportTree),
                          SlicingStatus::DEFAULT, logmsg);
    };
    ctl.stopcondition = [this]() { return canceled(); };
    ctl.cancelfn = [this]() { throw_if_canceled(); };

    po.m_supportdata->create_support_tree(ctl);

    if (!po.m_config.supports_enable.getBool()) return;

    throw_if_canceled();

    // Create the unified mesh
    auto rc = SlicingStatus::RELOAD_SCENE;

    // This is to prevent "Done." being displayed during merged_mesh()
    report_status(-1, L("Visualizing supports"));

    BOOST_LOG_TRIVIAL(debug) << "Processed support point count "
                             << po.m_supportdata->input.pts.size();

    // Check the mesh for later troubleshooting.
    if(po.support_mesh().empty())
        BOOST_LOG_TRIVIAL(warning) << "Support mesh is empty";

    report_status(-1, L("Visualizing supports"), rc);
}

void SLAPrint::Steps::generate_pad(SLAPrintObject &po) {
    // this step can only go after the support tree has been created
    // and before the supports had been sliced. (or the slicing has to be
    // repeated)

    if(po.m_config.pad_enable.getBool()) {
        // Get the distilled pad configuration from the config
        // (Again, despite it was retrieved in the previous step. Note that
        // on a param change event, the previous step might not be executed
        // depending on the specific parameter that has changed).
        sla::PadConfig pcfg = make_pad_cfg(po.m_config);
        po.m_supportdata->input.pad_cfg = pcfg;

//        if (!po.m_config.supports_enable.getBool() || pcfg.embed_object) {
//            // No support (thus no elevation) or zero elevation mode
//            // we sometimes call it "builtin pad" is enabled so we will
//            // get a sample from the bottom of the mesh and use it for pad
//            // creation.
//            sla::pad_blueprint(trmesh.its, bp, float(pad_h),
//                               float(po.m_config.layer_height.getFloat()),
//                               [this](){ throw_if_canceled(); });
//        }

        sla::JobController ctl;
        ctl.stopcondition = [this]() { return canceled(); };
        ctl.cancelfn = [this]() { throw_if_canceled(); };
        po.m_supportdata->create_pad(ctl);

        if (!validate_pad(po.m_supportdata->pad_mesh.its, pcfg))
            throw Slic3r::SlicingError(
                    L("No pad can be generated for this model with the "
                      "current configuration"));

    } else if(po.m_supportdata) {
        po.m_supportdata->pad_mesh = {};
    }

    throw_if_canceled();
    report_status(-1, L("Visualizing supports"), SlicingStatus::RELOAD_SCENE);
}

// Slicing the support geometries similarly to the model slicing procedure.
// If the pad had been added previously (see step "base_pool" than it will
// be part of the slices)
void SLAPrint::Steps::slice_supports(SLAPrintObject &po) {
    auto& sd = po.m_supportdata;

    if(sd) sd->support_slices.clear();

    // Don't bother if no supports and no pad is present.
    if (!po.m_config.supports_enable.getBool() && !po.m_config.pad_enable.getBool())
        return;

    if(sd) {
        auto heights = reserve_vector<float>(po.m_slice_index.size());

        for(auto& rec : po.m_slice_index) heights.emplace_back(rec.slice_level());

        sla::JobController ctl;
        ctl.stopcondition = [this]() { return canceled(); };
        ctl.cancelfn = [this]() { throw_if_canceled(); };

        sd->support_slices =
            sla::slice(sd->tree_mesh.its, sd->pad_mesh.its, heights,
                       float(po.config().slice_closing_radius.value), ctl);
    }

    for (size_t i = 0; i < sd->support_slices.size() && i < po.m_slice_index.size(); ++i)
        po.m_slice_index[i].set_support_slice_idx(po, i);

    apply_printer_corrections(po, soSupport);

    // Using RELOAD_SLA_PREVIEW to tell the Plater to pass the update
    // status to the 3D preview to load the SLA slices.
    report_status(-2, "", SlicingStatus::RELOAD_SLA_PREVIEW);
}

// get polygons for all instances in the object
static ExPolygons get_all_polygons(const SliceRecord& record, SliceOrigin o)
{
    if (!record.print_obj()) return {};

    ExPolygons polygons;
    auto &input_polygons = record.get_slice(o);
    auto &instances = record.print_obj()->instances();
    bool is_lefthanded = record.print_obj()->is_left_handed();
    polygons.reserve(input_polygons.size() * instances.size());

    for (const ExPolygon& polygon : input_polygons) {
        if(polygon.contour.empty()) continue;

        for (size_t i = 0; i < instances.size(); ++i)
        {
            ExPolygon poly;

            // We need to reverse if is_lefthanded is true but
            bool needreverse = is_lefthanded;

            // should be a move
            poly.contour.points.reserve(polygon.contour.size() + 1);

            auto& cntr = polygon.contour.points;
            if(needreverse)
                for(auto it = cntr.rbegin(); it != cntr.rend(); ++it)
                    poly.contour.points.emplace_back(it->x(), it->y());
            else
                for(auto& p : cntr)
                    poly.contour.points.emplace_back(p.x(), p.y());

            for(auto& h : polygon.holes) {
                poly.holes.emplace_back();
                auto& hole = poly.holes.back();
                hole.points.reserve(h.points.size() + 1);

                if(needreverse)
                    for(auto it = h.points.rbegin(); it != h.points.rend(); ++it)
                        hole.points.emplace_back(it->x(), it->y());
                else
                    for(auto& p : h.points)
                        hole.points.emplace_back(p.x(), p.y());
            }

            if(is_lefthanded) {
                for(auto& p : poly.contour) p.x() = -p.x();
                for(auto& h : poly.holes) for(auto& p : h) p.x() = -p.x();
            }

            poly.rotate(double(instances[i].rotation));
            poly.translate(Point{instances[i].shift.x(), instances[i].shift.y()});

            polygons.emplace_back(std::move(poly));
        }
    }

    return polygons;
}

void SLAPrint::Steps::initialize_printer_input()
{
    auto &printer_input = m_print->m_printer_input;

    // clear the rasterizer input
    printer_input.clear();

    size_t mx = 0;
    for(SLAPrintObject * o : m_print->m_objects) {
        if(auto m = o->get_slice_index().size() > mx) mx = m;
    }

    printer_input.reserve(mx);

    auto eps = coord_t(SCALED_EPSILON);

    for(SLAPrintObject * o : m_print->m_objects) {
        coord_t gndlvl = o->get_slice_index().front().print_level() - ilhs;

        for(const SliceRecord& slicerecord : o->get_slice_index()) {
            if (!slicerecord.is_valid())
                throw Slic3r::SlicingError(
                    L("There are unprintable objects. Try to "
                      "adjust support settings to make the "
                      "objects printable."));

            coord_t lvlid = slicerecord.print_level() - gndlvl;

            // Neat trick to round the layer levels to the grid.
            lvlid = eps * (lvlid / eps);

            auto it = std::lower_bound(printer_input.begin(),
                                       printer_input.end(),
                                       PrintLayer(lvlid));

            if(it == printer_input.end() || it->level() != lvlid)
                it = printer_input.insert(it, PrintLayer(lvlid));


            it->add(slicerecord);
        }
    }
}

// Merging the slices from all the print objects into one slice grid and
// calculating print statistics from the merge result.
void SLAPrint::Steps::merge_slices_and_eval_stats() {

    initialize_printer_input();

    auto &print_statistics = m_print->m_print_statistics;
    auto &printer_config   = m_print->m_printer_config;
    auto &material_config  = m_print->m_material_config;
    auto &printer_input    = m_print->m_printer_input;

    print_statistics.clear();

    const double area_fill = printer_config.area_fill.getFloat()*0.01;// 0.5 (50%);
    const double fast_tilt = printer_config.fast_tilt_time.getFloat();// 5.0;
    const double slow_tilt = printer_config.slow_tilt_time.getFloat();// 8.0;
    const double hv_tilt   = printer_config.high_viscosity_tilt_time.getFloat();// 10.0;

    const double init_exp_time = material_config.initial_exposure_time.getFloat();
    const double exp_time      = material_config.exposure_time.getFloat();

    const int fade_layers_cnt = m_print->m_default_object_config.faded_layers.getInt();// 10 // [3;20]

    const auto width          = scaled<double>(printer_config.display_width.getFloat());
    const auto height         = scaled<double>(printer_config.display_height.getFloat());
    const double display_area = width*height;

    double supports_volume(0.0);
    double models_volume(0.0);

    double estim_time(0.0);
    std::vector<double> layers_times;
    layers_times.reserve(printer_input.size());

    size_t slow_layers = 0;
    size_t fast_layers = 0;

    const double delta_fade_time = (init_exp_time - exp_time) / (fade_layers_cnt + 1);
    double fade_layer_time = init_exp_time;

    execution::SpinningMutex<ExecutionTBB> mutex;
    using Lock = std::lock_guard<decltype(mutex)>;

    // Going to parallel:
    auto printlayerfn = [this,
            // functions and read only vars
            area_fill, display_area, exp_time, init_exp_time, fast_tilt, slow_tilt, hv_tilt, material_config, delta_fade_time,

            // write vars
            &mutex, &models_volume, &supports_volume, &estim_time, &slow_layers,
            &fast_layers, &fade_layer_time, &layers_times](size_t sliced_layer_cnt)
    {
        PrintLayer &layer = m_print->m_printer_input[sliced_layer_cnt];

        // vector of slice record references
        auto& slicerecord_references = layer.slices();

        if(slicerecord_references.empty()) return;

        // Layer height should match for all object slices for a given level.
        const auto l_height = double(slicerecord_references.front().get().layer_height());

        // Calculation of the consumed material

        ExPolygons model_polygons;
        ExPolygons supports_polygons;

        size_t c = std::accumulate(layer.slices().begin(),
                                   layer.slices().end(),
                                   size_t(0),
                                   [](size_t a, const SliceRecord &sr) {
            return a + sr.get_slice(soModel).size();
        });

        model_polygons.reserve(c);

        c = std::accumulate(layer.slices().begin(),
                            layer.slices().end(),
                            size_t(0),
                            [](size_t a, const SliceRecord &sr) {
            return a + sr.get_slice(soSupport).size();
        });

        supports_polygons.reserve(c);

        for(const SliceRecord& record : layer.slices()) {

            ExPolygons modelslices = get_all_polygons(record, soModel);
            for(ExPolygon& p_tmp : modelslices) model_polygons.emplace_back(std::move(p_tmp));

            ExPolygons supportslices = get_all_polygons(record, soSupport);
            for(ExPolygon& p_tmp : supportslices) supports_polygons.emplace_back(std::move(p_tmp));

        }

        model_polygons = union_ex(model_polygons);
        double layer_model_area = 0;
        for (const ExPolygon& polygon : model_polygons)
            layer_model_area += area(polygon);

        if (layer_model_area < 0 || layer_model_area > 0) {
            Lock lck(mutex); models_volume += layer_model_area * l_height;
        }

        if(!supports_polygons.empty()) {
            if(model_polygons.empty()) supports_polygons = union_ex(supports_polygons);
            else supports_polygons = diff_ex(supports_polygons, model_polygons);
            // allegedly, union of subject is done withing the diff according to the pftPositive polyFillType
        }

        double layer_support_area = 0;
        for (const ExPolygon& polygon : supports_polygons)
            layer_support_area += area(polygon);

        if (layer_support_area < 0 || layer_support_area > 0) {
            Lock lck(mutex); supports_volume += layer_support_area * l_height;
        }

        // Here we can save the expensively calculated polygons for printing
        ExPolygons trslices;
        trslices.reserve(model_polygons.size() + supports_polygons.size());
        for(ExPolygon& poly : model_polygons) trslices.emplace_back(std::move(poly));
        for(ExPolygon& poly : supports_polygons) trslices.emplace_back(std::move(poly));

        layer.transformed_slices(union_ex(trslices));

        // Calculation of the slow and fast layers to the future controlling those values on FW

        const bool is_fast_layer = (layer_model_area + layer_support_area) <= display_area*area_fill;
        const double tilt_time = material_config.material_print_speed == slamsSlow              ? slow_tilt :
                                 material_config.material_print_speed == slamsHighViscosity     ? hv_tilt   :
                                 is_fast_layer ? fast_tilt : slow_tilt;

        { Lock lck(mutex);
            if (is_fast_layer)
                fast_layers++;
            else
                slow_layers++;

            // Calculation of the printing time

            double layer_times = 0.0;
            if (sliced_layer_cnt < 3)
                layer_times += init_exp_time;
            else if (fade_layer_time > exp_time) {
                fade_layer_time -= delta_fade_time;
                layer_times += fade_layer_time;
            }
            else
                layer_times += exp_time;
            layer_times += tilt_time;

            //// Per layer times (magical constants cuclulated from FW)

            static double exposure_safe_delay_before{ 3.0 };
            static double exposure_high_viscosity_delay_before{ 3.5 };
            static double exposure_slow_move_delay_before{ 1.0 };

            if (material_config.material_print_speed == slamsSlow)
                layer_times += exposure_safe_delay_before;
            else if (material_config.material_print_speed == slamsHighViscosity)
                layer_times += exposure_high_viscosity_delay_before;
            else if (!is_fast_layer)
                layer_times += exposure_slow_move_delay_before;

            // Increase layer time for "magic constants" from FW
            layer_times += (
                l_height * 5  // tower move
                + 120 / 1000  // Magical constant to compensate remaining computation delay in exposure thread
            );

            layers_times.push_back(layer_times);
            estim_time += layer_times;
        }
    };

    // sequential version for debugging:
    // for(size_t i = 0; i < m_printer_input.size(); ++i) printlayerfn(i);
    execution::for_each(ex_tbb, size_t(0), printer_input.size(), printlayerfn,
                        execution::max_concurrency(ex_tbb));

    auto SCALING2 = SCALING_FACTOR * SCALING_FACTOR;
    print_statistics.support_used_material = supports_volume * SCALING2;
    print_statistics.objects_used_material = models_volume  * SCALING2;

    // Estimated printing time
    // A layers count o the highest object
    if (printer_input.size() == 0)
        print_statistics.estimated_print_time = NaNd;
    else {
        print_statistics.estimated_print_time = estim_time;
        print_statistics.layers_times = layers_times;
    }

    print_statistics.fast_layers_count = fast_layers;
    print_statistics.slow_layers_count = slow_layers;

    report_status(-2, "", SlicingStatus::RELOAD_SLA_PREVIEW);
}

// Rasterizing the model objects, and their supports
void SLAPrint::Steps::rasterize()
{
    if(canceled() || !m_print->m_archiver) return;

    // coefficient to map the rasterization state (0-99) to the allocated
    // portion (slot) of the process state
    double sd = (100 - max_objstatus) / 100.0;

    // slot is the portion of 100% that is realted to rasterization
    unsigned slot = PRINT_STEP_LEVELS[slapsRasterize];

    // pst: previous state
    double pst = current_status();

    double increment = (slot * sd) / m_print->m_printer_input.size();
    double dstatus = current_status();

    execution::SpinningMutex<ExecutionTBB> slck;

    // procedure to process one height level. This will run in parallel
    auto lvlfn =
        [this, &slck, increment, &dstatus, &pst]
        (sla::RasterBase& raster, size_t idx)
    {
        PrintLayer& printlayer = m_print->m_printer_input[idx];
        if(canceled()) return;

        for (const ExPolygon& poly : printlayer.transformed_slices())
            raster.draw(poly);

        // Status indication guarded with the spinlock
        {
            std::lock_guard lck(slck);
            dstatus += increment;
            double st = std::round(dstatus);
            if(st > pst) {
                report_status(st, PRINT_STEP_LABELS(slapsRasterize));
                pst = st;
            }
        }
    };

    // last minute escape
    if(canceled()) return;

    // Print all the layers in parallel
    m_print->m_archiver->draw_layers(m_print->m_printer_input.size(), lvlfn,
                                    [this]() { return canceled(); }, ex_tbb);
}

std::string SLAPrint::Steps::label(SLAPrintObjectStep step)
{
    return OBJ_STEP_LABELS(step);
}

std::string SLAPrint::Steps::label(SLAPrintStep step)
{
    return PRINT_STEP_LABELS(step);
}

double SLAPrint::Steps::progressrange(SLAPrintObjectStep step) const
{
    return OBJ_STEP_LEVELS[step] * objectstep_scale;
}

double SLAPrint::Steps::progressrange(SLAPrintStep step) const
{
    return PRINT_STEP_LEVELS[step] * (100 - max_objstatus) / 100.0;
}

void SLAPrint::Steps::execute(SLAPrintObjectStep step, SLAPrintObject &obj)
{
    switch(step) {
    case slaposHollowing: hollow_model(obj); break;
    case slaposDrillHoles: drill_holes(obj); break;
    case slaposObjectSlice: slice_model(obj); break;
    case slaposSupportPoints:  support_points(obj); break;
    case slaposSupportTree: support_tree(obj); break;
    case slaposPad: generate_pad(obj); break;
    case slaposSliceSupports: slice_supports(obj); break;
    case slaposCount: assert(false);
    }
}

void SLAPrint::Steps::execute(SLAPrintStep step)
{
    switch (step) {
    case slapsMergeSlicesAndEval: merge_slices_and_eval_stats(); break;
    case slapsRasterize: rasterize(); break;
    case slapsCount: assert(false);
    }
}

}
