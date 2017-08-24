/* Copyright (c) 2008-2017 the MRtrix3 contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * MRtrix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/.
 */


#include "command.h"
#include "progressbar.h"
#include "thread_queue.h"
#include "algo/loop.h"
#include "transform.h"
#include "image.h"
#include "fixel/helpers.h"
#include "fixel/keys.h"
#include "fixel/loop.h"
#include "math/stats/glm.h"
#include "math/stats/import.h"
#include "math/stats/permutation.h"
#include "math/stats/typedefs.h"
#include "stats/cfe.h"
#include "stats/enhance.h"
#include "stats/permtest.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/mapping/mapper.h"
#include "dwi/tractography/mapping/loader.h"
#include "dwi/tractography/mapping/writer.h"


using namespace MR;
using namespace App;
using namespace MR::DWI::Tractography::Mapping;
using namespace MR::Math::Stats;
using Stats::CFE::direction_type;
using Stats::CFE::connectivity_value_type;

#define DEFAULT_CFE_DH 0.1
#define DEFAULT_CFE_E 2.0
#define DEFAULT_CFE_H 3.0
#define DEFAULT_CFE_C 0.5
#define DEFAULT_ANGLE_THRESHOLD 45.0
#define DEFAULT_CONNECTIVITY_THRESHOLD 0.01
#define DEFAULT_SMOOTHING_STD 10.0

void usage ()
{
  AUTHOR = "David Raffelt (david.raffelt@florey.edu.au) and Robert E. Smith (robert.smith@florey.edu.au)";

  SYNOPSIS = "Fixel-based analysis using connectivity-based fixel enhancement and non-parametric permutation testing";

  DESCRIPTION
      + Math::Stats::glm_column_ones_description;

  REFERENCES
  + "Raffelt, D.; Smith, RE.; Ridgway, GR.; Tournier, JD.; Vaughan, DN.; Rose, S.; Henderson, R.; Connelly, A." // Internal
    "Connectivity-based fixel enhancement: Whole-brain statistical analysis of diffusion MRI measures in the presence of crossing fibres. \n"
    "Neuroimage, 2015, 15(117):40-55\n"

  + "* If using the -nonstationary option: \n"
    "Salimi-Khorshidi, G. Smith, S.M. Nichols, T.E. \n"
    "Adjusting the effect of nonstationarity in cluster-based and TFCE inference. \n"
    "NeuroImage, 2011, 54(3), 2006-19\n" ;

  ARGUMENTS
  + Argument ("in_fixel_directory", "the fixel directory containing the data files for each subject (after obtaining fixel correspondence").type_file_in ()

  + Argument ("subjects", "a text file listing the subject identifiers (one per line). This should correspond with the filenames "
                          "in the fixel directory (including the file extension), and be listed in the same order as the rows of the design matrix.").type_image_in ()

  + Argument ("design", "the design matrix").type_file_in ()

  + Argument ("contrast", "the contrast matrix, specified as rows of weights").type_file_in ()

  + Argument ("tracks", "the tracks used to determine fixel-fixel connectivity").type_tracks_in ()

  + Argument ("out_fixel_directory", "the output directory where results will be saved. Will be created if it does not exist").type_text();


  OPTIONS

  + Stats::PermTest::Options (true)

  + OptionGroup ("Parameters for the Connectivity-based Fixel Enhancement algorithm")

  + Option ("cfe_dh", "the height increment used in the cfe integration (default: " + str(DEFAULT_CFE_DH, 2) + ")")
  + Argument ("value").type_float (0.001, 1.0)

  + Option ("cfe_e", "cfe extent exponent (default: " + str(DEFAULT_CFE_E, 2) + ")")
  + Argument ("value").type_float (0.0, 100.0)

  + Option ("cfe_h", "cfe height exponent (default: " + str(DEFAULT_CFE_H, 2) + ")")
  + Argument ("value").type_float (0.0, 100.0)

  + Option ("cfe_c", "cfe connectivity exponent (default: " + str(DEFAULT_CFE_C, 2) + ")")
  + Argument ("value").type_float (0.0, 100.0)

  + OptionGroup ("Additional options for fixelcfestats")

  + Option ("column", "add a column to the design matrix corresponding to subject fixel-wise values "
                      "(the contrast vector length must include columns for these additions)").allow_multiple()
  + Argument ("path").type_file_in()

  + Option ("smooth", "smooth the fixel value along the fibre tracts using a Gaussian kernel with the supplied FWHM (default: " + str(DEFAULT_SMOOTHING_STD, 2) + "mm)")
  + Argument ("FWHM").type_float (0.0, 200.0)

  + Option ("connectivity", "a threshold to define the required fraction of shared connections to be included in the neighbourhood (default: " + str(DEFAULT_CONNECTIVITY_THRESHOLD, 2) + ")")
  + Argument ("threshold").type_float (0.0, 1.0)

  + Option ("angle", "the max angle threshold for assigning streamline tangents to fixels (Default: " + str(DEFAULT_ANGLE_THRESHOLD, 2) + " degrees)")
  + Argument ("value").type_float (0.0, 90.0);

}



template <class VectorType>
void write_fixel_output (const std::string& filename,
                         const VectorType& data,
                         const Header& header)
{
  auto output = Image<float>::create (filename, header);
  for (uint32_t i = 0; i < data.size(); ++i) {
    output.index(0) = i;
    output.value() = data[i];
  }
}



// Define data importer class that willl obtain fixel data for a
//   specific subject based on the string path to the image file for
//   that subject
class SubjectFixelImport : public SubjectDataImportBase
{ MEMALIGN(SubjectFixelImport)
  public:
    SubjectFixelImport (const std::string& path) :
        SubjectDataImportBase (path),
        H (Header::open (path)),
        data (H.get_image<float>())
    {
      for (size_t axis = 1; axis < data.ndim(); ++axis) {
        if (data.size(axis) > 1)
          throw Exception ("Image file \"" + path + "\" does not contain fixel data (wrong dimensions)");
      }
    }

    void operator() (matrix_type::ColXpr column) const override
    {
      assert (column.rows() == size());
      Image<float> temp (data); // For thread-safety
      column = temp.row(0);
    }

    default_type operator[] (const size_t index) const override
    {
      assert (index < size());
      Image<float> temp (data); // For thread-safety
      temp.index(0) = index;
      return default_type(temp.value());
    }

    size_t size() const override { return data.size(0); }

    const Header& header() const { return H; }

  private:
    Header H;
    const Image<float> data;

};



void run()
{

  const value_type cfe_dh = get_option_value ("cfe_dh", DEFAULT_CFE_DH);
  const value_type cfe_h = get_option_value ("cfe_h", DEFAULT_CFE_H);
  const value_type cfe_e = get_option_value ("cfe_e", DEFAULT_CFE_E);
  const value_type cfe_c = get_option_value ("cfe_c", DEFAULT_CFE_C);
  int num_perms = get_option_value ("nperms", DEFAULT_NUMBER_PERMUTATIONS);
  const value_type smooth_std_dev = get_option_value ("smooth", DEFAULT_SMOOTHING_STD) / 2.3548;
  const value_type connectivity_threshold = get_option_value ("connectivity", DEFAULT_CONNECTIVITY_THRESHOLD);
  const bool do_nonstationary_adjustment = get_options ("nonstationary").size();
  int nperms_nonstationary = get_option_value ("nperms_nonstationary", DEFAULT_NUMBER_PERMUTATIONS_NONSTATIONARITY);
  const value_type angular_threshold = get_option_value ("angle", DEFAULT_ANGLE_THRESHOLD);


  const std::string input_fixel_directory = argument[0];
  Header index_header = Fixel::find_index_header (input_fixel_directory);
  auto index_image = index_header.get_image<uint32_t>();

  const uint32_t num_fixels = Fixel::get_number_of_fixels (index_header);
  CONSOLE ("number of fixels: " + str(num_fixels));

  vector<Eigen::Vector3> positions (num_fixels);
  vector<direction_type> directions (num_fixels);

  const std::string output_fixel_directory = argument[5];
  Fixel::copy_index_and_directions_file (input_fixel_directory, output_fixel_directory);

  {
    auto directions_data = Fixel::find_directions_header (input_fixel_directory).get_image<default_type>().with_direct_io ({+2,+1});
    // Load template fixel directions
    Transform image_transform (index_image);
    for (auto i = Loop ("loading template fixel directions and positions", index_image, 0, 3)(index_image); i; ++i) {
      const Eigen::Vector3 vox ((default_type)index_image.index(0), (default_type)index_image.index(1), (default_type)index_image.index(2));
      index_image.index(3) = 1;
      uint32_t offset = index_image.value();
      size_t fixel_index = 0;
      for (auto f = Fixel::Loop (index_image) (directions_data); f; ++f, ++fixel_index) {
        directions[offset + fixel_index] = directions_data.row(1);
        positions[offset + fixel_index] = image_transform.voxel2scanner * vox;
      }
    }
  }

  // Read file names and check files exist
  CohortDataImport importer;
  importer.initialise<SubjectFixelImport> (argument[1]);
  for (size_t i = 0; i != importer.size(); ++i) {
    if (!Fixel::fixels_match (index_header, dynamic_cast<SubjectFixelImport*>(importer[i].get())->header()))
      throw Exception ("Fixel data file \"" + importer[i]->name() + "\" does not match template fixel image");
  }
  CONSOLE ("Number of subjects: " + str(importer.size()));

  // Load design matrix:
  const matrix_type design = load_matrix (argument[2]);
  CONSOLE ("design matrix dimensions: " + str(design.rows()) + " x " + str(design.cols()));
  if (design.rows() != (ssize_t)importer.size())
    throw Exception ("number of input files does not match number of rows in design matrix");

  // Load permutations file if supplied
  auto opt = get_options("permutations");
  vector<vector<size_t> > permutations;
  if (opt.size()) {
    permutations = Math::Stats::Permutation::load_permutations_file (opt[0][0]);
    num_perms = permutations.size();
    if (permutations[0].size() != (size_t)design.rows())
      throw Exception ("number of rows in the permutations file (" + str(opt[0][0]) + ") does not match number of rows in design matrix");
  }

  // Load non-stationary correction permutations file if supplied
  opt = get_options("permutations_nonstationary");
  vector<vector<size_t> > permutations_nonstationary;
  if (opt.size()) {
    if (do_nonstationary_adjustment) {
      permutations_nonstationary = Math::Stats::Permutation::load_permutations_file (opt[0][0]);
      nperms_nonstationary = permutations_nonstationary.size();
      if (permutations_nonstationary[0].size() != (size_t)design.rows())
        throw Exception ("number of rows in the nonstationary permutations file (" + str(opt[0][0]) + ") does not match number of rows in design matrix");
    } else {
      WARN ("-permutations_nonstationary option ignored: nonstationarity correction is not being performed (-nonstationary option)");
    }
  }

  // Load contrast matrix
  const matrix_type contrast = load_matrix (argument[3]);
  const size_t num_contrasts = contrast.rows();

  // Before validating the contrast matrix, we first need to see if there are any
  //   additional design matrix columns coming from fixel-wise subject data
  vector<CohortDataImport> extra_columns;
  bool nans_in_columns = false;
  opt = get_options ("column");
  for (size_t i = 0; i != opt.size(); ++i) {
    extra_columns.push_back (CohortDataImport());
    extra_columns[i].initialise<SubjectFixelImport> (opt[i][0]);
    if (!extra_columns[i].allFinite())
      nans_in_columns = true;
  }
  if (extra_columns.size()) {
    CONSOLE ("number of element-wise design matrix columns: " + str(extra_columns.size()));
    if (nans_in_columns)
      INFO ("Non-finite values detected in element-wise design matrix columns; individual rows will be removed from fixel-wise design matrices accordingly");
  }

  if (contrast.cols() != design.cols() + ssize_t(extra_columns.size()))
    throw Exception ("the number of columns per contrast (" + str(contrast.cols()) + ")"
                     + (extra_columns.size() ? " (in addition to the " + str(extra_columns.size()) + " uses of -column)" : "")
                     + " does not equal the number of columns in the design matrix (" + str(design.cols()) + ")");

  // Compute fixel-fixel connectivity
  vector<std::map<uint32_t, Stats::CFE::connectivity> > connectivity_matrix (num_fixels);
  vector<uint16_t> fixel_TDI (num_fixels, 0.0);
  const std::string track_filename = argument[4];
  DWI::Tractography::Properties properties;
  DWI::Tractography::Reader<float> track_file (track_filename, properties);
  // Read in tracts, and compute whole-brain fixel-fixel connectivity
  const size_t num_tracks = properties["count"].empty() ? 0 : to<int> (properties["count"]);
  if (!num_tracks)
    throw Exception ("no tracks found in input file");
  if (num_tracks < 1000000) {
    WARN ("more than 1 million tracks is preferable to ensure robust fixel-fixel connectivity; file \"" + track_filename + "\" contains only " + str(num_tracks));
  }
  {
    typedef DWI::Tractography::Mapping::SetVoxelDir SetVoxelDir;
    DWI::Tractography::Mapping::TrackLoader loader (track_file, num_tracks, "pre-computing fixel-fixel connectivity");
    DWI::Tractography::Mapping::TrackMapperBase mapper (index_image);
    mapper.set_upsample_ratio (DWI::Tractography::Mapping::determine_upsample_ratio (index_header, properties, 0.333f));
    mapper.set_use_precise_mapping (true);
    Stats::CFE::TrackProcessor tract_processor (index_image, directions, fixel_TDI, connectivity_matrix, angular_threshold);
    Thread::run_queue (
        loader,
        Thread::batch (DWI::Tractography::Streamline<float>()),
        mapper,
        Thread::batch (SetVoxelDir()),
        tract_processor);
  }
  track_file.close();

  // Normalise connectivity matrix and threshold, pre-compute fixel-fixel weights for smoothing.
  vector<std::map<uint32_t, connectivity_value_type> > smoothing_weights (num_fixels);
  bool do_smoothing = false;

  const float gaussian_const2 = 2.0 * smooth_std_dev * smooth_std_dev;
  float gaussian_const1 = 1.0;
  if (smooth_std_dev > 0.0) {
    do_smoothing = true;
    gaussian_const1 = 1.0 / (smooth_std_dev *  std::sqrt (2.0 * Math::pi));
  }

  {
    // TODO This could trivially be multi-threaded; fixels are handled independently
    ProgressBar progress ("normalising and thresholding fixel-fixel connectivity matrix", num_fixels);
    for (uint32_t fixel = 0; fixel < num_fixels; ++fixel) {

      auto it = connectivity_matrix[fixel].begin();
      while (it != connectivity_matrix[fixel].end()) {
        const connectivity_value_type connectivity = it->second.value / connectivity_value_type (fixel_TDI[fixel]);
        if (connectivity < connectivity_threshold) {
          connectivity_matrix[fixel].erase (it++);
        } else {
          if (do_smoothing) {
            const value_type distance = std::sqrt (Math::pow2 (positions[fixel][0] - positions[it->first][0]) +
                                                   Math::pow2 (positions[fixel][1] - positions[it->first][1]) +
                                                   Math::pow2 (positions[fixel][2] - positions[it->first][2]));
            const connectivity_value_type smoothing_weight = connectivity * gaussian_const1 * std::exp (-std::pow (distance, 2) / gaussian_const2);
            if (smoothing_weight > 0.01)
              smoothing_weights[fixel].insert (std::pair<uint32_t, connectivity_value_type> (it->first, smoothing_weight));
          }
          // Here we pre-exponentiate each connectivity value by C
          it->second.value = std::pow (connectivity, cfe_c);
          ++it;
        }
      }
      // Make sure the fixel is fully connected to itself
      connectivity_matrix[fixel].insert (std::pair<uint32_t, Stats::CFE::connectivity> (fixel, Stats::CFE::connectivity (1.0)));
      smoothing_weights[fixel].insert (std::pair<uint32_t, connectivity_value_type> (fixel, gaussian_const1));

      // Normalise smoothing weights
      value_type sum = 0.0;
      for (auto smooth_it = smoothing_weights[fixel].begin(); smooth_it != smoothing_weights[fixel].end(); ++smooth_it)
        sum += smooth_it->second;
      const value_type norm_factor = 1.0 / sum;
      for (auto smooth_it = smoothing_weights[fixel].begin(); smooth_it != smoothing_weights[fixel].end(); ++smooth_it)
        smooth_it->second *= norm_factor;
      progress++;
    }
  }

  Header output_header (dynamic_cast<SubjectFixelImport*>(importer[0].get())->header());
  output_header.keyval()["num permutations"] = str(num_perms);
  output_header.keyval()["dh"] = str(cfe_dh);
  output_header.keyval()["cfe_e"] = str(cfe_e);
  output_header.keyval()["cfe_h"] = str(cfe_h);
  output_header.keyval()["cfe_c"] = str(cfe_c);
  output_header.keyval()["angular threshold"] = str(angular_threshold);
  output_header.keyval()["connectivity threshold"] = str(connectivity_threshold);
  output_header.keyval()["smoothing FWHM"] = str(smooth_std_dev * 2.3548);


  // Load input data
  matrix_type data = matrix_type::Zero (num_fixels, importer.size());
  bool nans_in_data = false;
  {
    ProgressBar progress ("loading input images", importer.size());
    for (size_t subject = 0; subject < importer.size(); subject++) {
      (*importer[subject]) (data.col (subject));
      // Smooth the data
      vector_type smoothed_data (vector_type::Zero (num_fixels));
      for (size_t fixel = 0; fixel < num_fixels; ++fixel) {
        if (std::isfinite (data (fixel, subject))) {
          value_type value = 0.0, sum_weights = 0.0;
          std::map<uint32_t, connectivity_value_type>::const_iterator it = smoothing_weights[fixel].begin();
          for (; it != smoothing_weights[fixel].end(); ++it) {
            if (std::isfinite (data (it->first, subject))) {
              value += data (it->first, subject) * it->second;
              sum_weights += it->second;
            }
          }
          if (sum_weights)
            smoothed_data (fixel) = value / sum_weights;
          else
            smoothed_data (fixel) = NaN;
        } else {
          smoothed_data (fixel) = NaN;
        }
      }
      data.col (subject) = smoothed_data;
      if (!smoothed_data.allFinite())
        nans_in_data = true;
    }
    progress++;
  }
  if (nans_in_data) {
    INFO ("Non-finite values present in data; rows will be removed from fixel-wise design matrices accordingly");
    if (!extra_columns.size()) {
      INFO ("(Note that this will result in slower execution than if such values were not present)");
    }
  }


  // Construct the class for performing the initial statistical tests
  std::shared_ptr<GLMTestBase> glm_test;
  if (extra_columns.size() || nans_in_data) {
    glm_test.reset (new GLMTTestVariable (extra_columns, data, design, contrast, nans_in_data, nans_in_columns));
  } else {
    glm_test.reset (new GLMTTestFixed (data, design, contrast));
  }

  // Only add contrast row number to image outputs if there's more than one contrast
  auto postfix = [&] (const size_t i) { return (num_contrasts > 1) ? ("_" + str(i)) : ""; };

  {
    matrix_type betas (contrast.cols(), num_fixels);
    matrix_type abs_effect_size (num_contrasts, num_fixels), std_effect_size (num_contrasts, num_fixels), stdev (num_contrasts, num_fixels);

    if (extra_columns.size()) {

      // For each variable of interest (e.g. beta coefficients, effect size etc.) need to:
      //   Construct the output data vector, with size = num_fixels
      //   For each fixel:
      //     Use glm_test to obtain the design matrix for the default permutation for that fixel
      //     Use the relevant Math::Stats::GLM function to get the value of interest for just that fixel
      //       (will still however need to come out as a matrix_type)
      //     Write that value to data vector
      //   Finally, use write_fixel_output() function to write to an image file
      class Source
      { NOMEMALIGN
        public:
          Source (const size_t num_fixels) :
              num_fixels (num_fixels),
              counter (0),
              progress (new ProgressBar ("calculating basic properties of default permutation", num_fixels)) { }

          bool operator() (size_t& fixel_index)
          {
            fixel_index = counter++;
            if (counter >= num_fixels) {
              progress.reset();
              return false;
            }
            assert (progress);
            ++(*progress);
            return true;
          }

        private:
          const size_t num_fixels;
          size_t counter;
          std::unique_ptr<ProgressBar> progress;
      };

      class Functor
      { MEMALIGN(Functor)
        public:
          Functor (const matrix_type& data, std::shared_ptr<GLMTestBase> glm_test, const matrix_type& contrasts,
                   matrix_type& betas, matrix_type& abs_effect_size, matrix_type& std_effect_size, matrix_type& stdev) :
              data (data),
              glm_test (glm_test),
              contrasts (contrasts),
              global_betas (betas),
              global_abs_effect_size (abs_effect_size),
              global_std_effect_size (std_effect_size),
              global_stdev (stdev) { }

          bool operator() (const size_t& fixel_index)
          {
            const matrix_type data_fixel = data.row (fixel_index);
            const matrix_type design_fixel = dynamic_cast<GLMTTestVariable*>(glm_test.get())->default_design (fixel_index);
            Math::Stats::GLM::all_stats (data_fixel, design_fixel, contrasts,
                                         local_betas, local_abs_effect_size, local_std_effect_size, local_stdev);
            global_betas.col(fixel_index) = local_betas;
            global_abs_effect_size.col(fixel_index) = local_abs_effect_size.col(0);
            global_std_effect_size.col(fixel_index) = local_std_effect_size.col(0);
            global_stdev.col(fixel_index) = local_stdev.col(0);
            return true;
          }

        private:
          const matrix_type& data;
          const std::shared_ptr<GLMTestBase> glm_test;
          const matrix_type& contrasts;
          matrix_type& global_betas;
          matrix_type& global_abs_effect_size;
          matrix_type& global_std_effect_size;
          matrix_type& global_stdev;
          matrix_type local_betas, local_abs_effect_size, local_std_effect_size, local_stdev;
      };

      Source source (num_fixels);
      Functor functor (data, glm_test, contrast,
                       betas, abs_effect_size, std_effect_size, stdev);
      Thread::run_queue (source, Thread::batch (size_t()), Thread::multi (functor));

    } else {

      ProgressBar progress ("calculating basic properties of default permutation");
      Math::Stats::GLM::all_stats (data, design, contrast,
                                   betas, abs_effect_size, std_effect_size, stdev);
    }

    ProgressBar progress ("outputting beta coefficients, effect size and standard deviation", contrast.cols() + (3 * num_contrasts));
    for (ssize_t i = 0; i != contrast.cols(); ++i) {
      write_fixel_output (Path::join (output_fixel_directory, "beta" + str(i) + ".mif"), betas.row(i), output_header);
      ++progress;
    }
    for (size_t i = 0; i != num_contrasts; ++i) {
      write_fixel_output (Path::join (output_fixel_directory, "abs_effect" + postfix(i) + ".mif"), abs_effect_size.row(i), output_header); ++progress;
      write_fixel_output (Path::join (output_fixel_directory, "std_effect" + postfix(i) + ".mif"), std_effect_size.row(i), output_header); ++progress;
      write_fixel_output (Path::join (output_fixel_directory, "std_dev" + postfix(i) + ".mif"), stdev.row(i), output_header);
    }

  }

  // Construct the class for performing fixel-based statistical enhancement
  std::shared_ptr<Stats::EnhancerBase> cfe_integrator (new Stats::CFE::Enhancer (connectivity_matrix, cfe_dh, cfe_e, cfe_h));

  // If performing non-stationarity adjustment we need to pre-compute the empirical CFE statistic
  matrix_type empirical_cfe_statistic;
  if (do_nonstationary_adjustment) {
    empirical_cfe_statistic = vector_type::Zero (num_fixels);
    if (permutations_nonstationary.size()) {
      Stats::PermTest::PermutationStack permutations (permutations_nonstationary, "precomputing empirical statistic for non-stationarity adjustment");
      Stats::PermTest::precompute_empirical_stat (glm_test, cfe_integrator, permutations, empirical_cfe_statistic);
    } else {
      Stats::PermTest::PermutationStack permutations (nperms_nonstationary, design.rows(), "precomputing empirical statistic for non-stationarity adjustment", false);
      Stats::PermTest::precompute_empirical_stat (glm_test, cfe_integrator, permutations, empirical_cfe_statistic);
    }
    output_header.keyval()["nonstationary adjustment"] = str(true);
    for (size_t i = 0; i != num_contrasts; ++i)
      write_fixel_output (Path::join (output_fixel_directory, "cfe_empirical" + postfix(i) + ".mif"), empirical_cfe_statistic.row(i), output_header);
  } else {
    output_header.keyval()["nonstationary adjustment"] = str(false);
  }

  // Precompute default statistic and CFE statistic
  matrix_type cfe_output (num_contrasts, num_fixels);
  matrix_type tvalue_output (num_contrasts, num_fixels);

  Stats::PermTest::precompute_default_permutation (glm_test, cfe_integrator, empirical_cfe_statistic, cfe_output, tvalue_output);

  for (size_t i = 0; i != num_contrasts; ++i) {
    write_fixel_output (Path::join (output_fixel_directory, "cfe" + postfix(i) + ".mif"), cfe_output.row(i), output_header);
    write_fixel_output (Path::join (output_fixel_directory, "tvalue" + postfix(i) + ".mif"), tvalue_output.row(i), output_header);
  }

  // Perform permutation testing
  if (!get_options ("notest").size()) {
    matrix_type perm_distribution (num_contrasts, num_perms);
    matrix_type uncorrected_pvalues (num_contrasts, num_fixels);

    if (permutations.size()) {
      Stats::PermTest::run_permutations (permutations, glm_test, cfe_integrator, empirical_cfe_statistic,
                                         cfe_output, perm_distribution, uncorrected_pvalues);
    } else {
      Stats::PermTest::run_permutations (num_perms, glm_test, cfe_integrator, empirical_cfe_statistic,
                                         cfe_output, perm_distribution, uncorrected_pvalues);
    }

    ProgressBar progress ("outputting final results");
    for (size_t i = 0; i != num_contrasts; ++i) {
      save_vector (perm_distribution.row(i), Path::join (output_fixel_directory, "perm_dist" + postfix(i) + ".txt"));
      ++progress;
    }

    matrix_type pvalue_output (num_contrasts, num_fixels);
    Math::Stats::Permutation::statistic2pvalue (perm_distribution, cfe_output, pvalue_output);
    ++progress;
    for (size_t i = 0; i != num_contrasts; ++i) {
      write_fixel_output (Path::join (output_fixel_directory, "fwe_pvalue" + postfix(i) + ".mif"), pvalue_output.row(i), output_header);
      ++progress;
      write_fixel_output (Path::join (output_fixel_directory, "uncorrected_pvalue" + postfix(i) + ".mif"), uncorrected_pvalues.row(i), output_header);
      ++progress;
    }

  }
}
