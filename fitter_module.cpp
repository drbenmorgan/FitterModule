// Ourselves:
#include <fitter_module.h>

// Standard library:
#include <iostream>
#include <stdexcept>

// geomtools
#include "bayeux/geomtools/helix_3d.h"
#include "bayeux/geomtools/line_3d.h"

// falaise
#include <falaise/snemo/datamodels/calibrated_data.h>
#include <falaise/snemo/datamodels/data_model.h>
#include <falaise/snemo/datamodels/tracker_clustering_data.h>
#include <falaise/snemo/datamodels/tracker_trajectory.h>
#include <falaise/snemo/datamodels/tracker_trajectory_data.h>
#include <falaise/snemo/datamodels/tracker_trajectory_solution.h>
#include "falaise/snemo/datamodels/base_trajectory_pattern.h"
#include "falaise/snemo/datamodels/calibrated_calorimeter_hit.h"

// Registration instantiation macro :
DPP_MODULE_REGISTRATION_IMPLEMENT(fitter_module, "fitter_module")

// Constructor :
fitter_module::fitter_module(datatools::logger::priority logging_priority_)
    : dpp::base_module(logging_priority_) {}

// Destructor :
fitter_module::~fitter_module() {
  // MUST reset module at destruction because DPP is idiotic
  reset();
}

void fitter_module::reset() {
  _TTD_label_.clear();
  eventCounter = 0;
  this->_set_initialized(false);
}

void fitter_module::initialize(const datatools::properties &setup_,
                               datatools::service_manager &service_manager_,
                               dpp::module_handle_dict_type & /* module_dict_ */) {
  DT_THROW_IF(this->is_initialized(), std::logic_error,
              "Module 'Fitter' is already initialized ! ");

  dpp::base_module::_common_initialize(setup_);

  // Look for services
  if (service_manager_.has("geometry")) {
    const auto &GS = service_manager_.get<geomtools::geometry_service>("geometry");

    // initialize geometry manager
    //    std::cout << "Initialize geo manager " << std::endl;
    geometry_manager_ = &GS.get_geom_manager();
    DT_THROW_IF(!geometry_manager_, std::runtime_error,
                "Null pointer to geometry manager return by geometry_service");
  }

  // check the label
  _TTD_label_ = snemo::datamodel::data_info::default_tracker_trajectory_data_label();
  _TCD_label_ = snemo::datamodel::data_info::default_tracker_clustering_data_label();

  eventCounter = 0;
  this->_set_initialized(true);
}

// Processing :
dpp::base_module::process_status fitter_module::process(datatools::things &data_record_) {
  DT_THROW_IF(!this->is_initialized(), std::logic_error, "Module 'Fitter' is not initialized !");

  // Check input tracker clustering data //
  namespace sdm = snemo::datamodel;

  sdm::tracker_clustering_data *the_cluster_data = nullptr;
  if (data_record_.has(_TCD_label_)) {
    the_cluster_data = &(data_record_.grab<sdm::tracker_clustering_data>(_TCD_label_));
    if (!the_cluster_data->has_solutions()) {
      return dpp::base_module::PROCESS_INVALID;
    }
  } else {
    return dpp::base_module::PROCESS_INVALID;
  }

  // Check output tracker trajectory data //
  sdm::tracker_trajectory_data *the_trajectory_data = nullptr;
  if (data_record_.has(_TTD_label_)) {
    the_trajectory_data = &(data_record_.grab<sdm::tracker_trajectory_data>(_TTD_label_));
  } else {
    the_trajectory_data = &(data_record_.add<sdm::tracker_trajectory_data>(_TTD_label_));
  }

  bool preserve_former_output = true;  // keep all
  if (!preserve_former_output) {
    the_trajectory_data->reset();
  }

  /********************
   * Process the data *
   ********************/
  // Inputs: "the_cluster_data" snemo::datamodel::tracker_clustering_data*
  //       : "the_trajectory_data"  snemo::datamodel::tracker_trajectory_data&

  // Process events for trajectory consolidation
  // make a trajectory solution
  auto htts = datatools::make_handle<sdm::tracker_trajectory_solution>();
  the_trajectory_data->add_solution(htts, true);
  // WTAFx2?
  the_trajectory_data->grab_solutions().back().grab().set_solution_id(
      the_trajectory_data->get_number_of_solutions() - 1);
  sdm::tracker_trajectory_solution &trajectory_solution =
      the_trajectory_data->grab_solutions().back().grab();  // maybe store in here a bit

  // Process clusters hits for fitting
  std::cout << "In process: event counter = " << eventCounter << std::endl;

  // Iterate over all solutions
  int nsol = 0;
  int ncl = 0;

  for (auto const &clusterSolution : the_cluster_data->get_solutions()) {
    std::cout << "cluster solution number: " << ++nsol << std::endl;

    for (auto const &cluster : clusterSolution->get_clusters()) {
      const sdm::calibrated_tracker_hit::collection_type &gg_hits_col = cluster->get_hits();
      ncl = cluster->get_cluster_id();

      std::cout << "cluster number: " << ncl << std::endl;
      std::cout << "number of gg hits: " << gg_hits_col.size() << std::endl;

      std::vector<TrackerHit> rings;
      rings.reserve(gg_hits_col.size());

      for (const auto &hit : gg_hits_col) {
        // work with geiger hits as members of a given cluster
        GeigerRing ring{hit->get_r(), hit->get_x(),       hit->get_y(),
                        hit->get_z(), hit->get_sigma_r(), hit->get_sigma_z()};
        MetaInfo mi{hit->get_id(), hit->get_side(), hit->get_row(), hit->get_layer()};

        rings.emplace_back(TrackerHit{ncl, ring, mi});
      }

      // ready to fit
      SNFitter snf{rings};
      std::vector<HelixFit> hres = snf.fithelix();
      std::vector<LineFit> lres = snf.fitline();
      std::vector<BrokenLineFit> bres = snf.fitbrokenline();

      // try to do something with the results
      // Line first
      for (const LineFit& entry : lres) {
        if (entry.status > 1) {
          std::cout << "This line fit failed with status " << entry.status << std::endl;
        }
        else {
          std::cout << "Line fit: (status, chi2) " << entry.status << ", " << entry.chi2
                    << std::endl;
          std::cout << "slope, intercept in xy " << entry.slxy << ", " << entry.ixy << std::endl;
          std::cout << "errors in xy " << entry.errslxy << ", " << entry.errixy << std::endl;
          std::cout << "slope, intercept in xz " << entry.slxz << ", " << entry.ixz << std::endl;
          std::cout << "errors in xz " << entry.errslxz << ", " << entry.errixz << std::endl;
        }
      }

      // Helix
      std::cout << "*** Helix next ***" << std::endl;
      for (const HelixFit& entry : hres) {
        if (entry.status > 1) {
          std::cout << "This helix fit failed with status " << entry.status << std::endl;
        }
        else {
          std::cout << "Helix fit: (status, chi2) " << entry.status << ", " << entry.chi2
                    << std::endl;
          std::cout << "radius and pitch " << entry.radius << ", " << entry.pitch << std::endl;
          std::cout << "errors in r, p " << entry.raderr << ", " << entry.errpitch << std::endl;
          std::cout << "centre (x,y,z) " << entry.xc << ", " << entry.yc << ", " << entry.zc
                    << std::endl;
          std::cout << "centre errors " << entry.errxc << ", " << entry.erryc << ", " << entry.errzc
                    << std::endl;
        }
      }

      // Broken Line last
      std::cout << "*** Broken Line section next ***" << std::endl;
      for (const BrokenLineFit& entry : bres) {
        if (entry.status > 1) {
          std::cout << "This broken line fit failed with status " << entry.status << std::endl;
        }
        else {
          LineFit lf = entry.linefit1;
          LineFit lf2 = entry.linefit2;
          if (lf2.chi2 < 0.0) {  // just one linefit, no break
            std::cout << "Broken Line fit: (status, chi2) " << entry.status << ", " << entry.chi2
                      << std::endl;
            std::cout << "slope, intercept in xy " << lf.slxy << ", " << lf.ixy << std::endl;
            std::cout << "errors in xy " << lf.errslxy << ", " << lf.errixy << std::endl;
            std::cout << "slope, intercept in xz " << lf.slxz << ", " << lf.ixz << std::endl;
            std::cout << "errors in xz " << lf.errslxz << ", " << lf.errixz << std::endl;
          } else {  // two lines fitted from break
            std::cout << "Broken Line fit: (status, chi2) " << entry.status << ", " << entry.chi2
                      << std::endl;
            std::cout << "(1) slope, intercept in xy " << lf.slxy << ", " << lf.ixy << std::endl;
            std::cout << "(1) errors in xy " << lf.errslxy << ", " << lf.errixy << std::endl;
            std::cout << "(1) slope, intercept in xz " << lf.slxz << ", " << lf.ixz << std::endl;
            std::cout << "(1) errors in xz " << lf.errslxz << ", " << lf.errixz << std::endl;
            std::cout << "Second line " << std::endl;
            std::cout << "(2) slope, intercept in xy " << lf2.slxy << ", " << lf2.ixy << std::endl;
            std::cout << "(2) errors in xy " << lf2.errslxy << ", " << lf2.errixy << std::endl;
            std::cout << "(2) slope, intercept in xz " << lf2.slxz << ", " << lf2.ixz << std::endl;
            std::cout << "(2) errors in xz " << lf2.errslxz << ", " << lf2.errixz << std::endl;
          }
        }
      }
    }
  }

  eventCounter++;
  return dpp::base_module::PROCESS_SUCCESS;
}
