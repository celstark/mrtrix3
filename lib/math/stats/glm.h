/*
    Copyright 2011 Brain Research Institute, Melbourne, Australia

    Written by David Raffelt and Donald Tournier 23/07/11.

    This file is part of MRtrix.

    MRtrix is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MRtrix is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __math_stats_glm_h__
#define __math_stats_glm_h__

#include "types.h"
#include "math/least_squares.h"

#define GLM_BATCH_SIZE 1024

namespace MR
{
  namespace Math
  {
    namespace Stats
    {

      namespace GLM {

        //! scale contrasts for use in t-test
        /*! Note each row of the contrast matrix will be treated as an independent contrast. */
        template <typename ValueType>
          inline Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> scale_contrasts (const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& contrasts,
                                                                                           const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design,
                                                                                           size_t degrees_of_freedom)
          {
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> XtX = Math::pinv ((design.transpose() * design).template cast<double>()).template cast<ValueType>();

            // make sure contrast is a column vector:
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> scaled_contrasts (contrasts);
            if (scaled_contrasts.cols() > 1 && scaled_contrasts.rows() > 1)
              throw Exception ("too many columns in contrast matrix: this implementation currently only supports univariate GLM");
            if (scaled_contrasts.rows() > 1)
              scaled_contrasts.transposeInPlace();
            scaled_contrasts.resize (scaled_contrasts.rows(), design.cols());

            for (size_t n = 0; n < contrasts.rows(); ++n) {
              Eigen::Matrix<ValueType, Eigen::Dynamic, 1> pinv_XtX_c = XtX * contrasts.row(n).transpose(); //TODO transpose
              scaled_contrasts.row(n) *= std::sqrt (ValueType(degrees_of_freedom) / contrasts.row(n).dot(pinv_XtX_c));
            }

            return scaled_contrasts;
          }



        //! generic GLM t-test
        /*! note that the data, effects, and residual matrices are transposed.
         * This is to take advantage of the GSL's convention of storing
         * matrices in column-major format.  TODO check Eigen default stride
         *
         * Note also that the contrast matrix should already have been scaled
         * using the GLM::scale_contrasts() function. */
        template <typename ValueType>
          inline void ttest (
              Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& tvalues,
              const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design,
              const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& pinv_design,
              const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& measurements,
              const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& scaled_contrasts,
              Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& betas,
              Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& residuals)
          {
            betas.noalias() = measurements * pinv_design;
            residuals.noalias() = measurements - betas * design;
            tvalues.noalias() = betas * scaled_contrasts;
            for (size_t n = 0; n < tvalues.rows(); ++n)
              tvalues.row(n).array() /= residuals.row(n).norm();
          }

          /** \addtogroup Statistics
          @{ */
          /*! Compute a matrix of the beta coefficients
          * @param measurements a matrix storing the measured data for each subject in a column
          * @param design the design matrix (unlike other packages a column of ones is NOT automatically added for correlation analysis)
          * @param betas the matrix containing the output effect
          */
          template <typename ValueType>
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> solve_betas (const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& measurements,
                                                                                  const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design) {
            return design.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(measurements.transpose());
          }



          /*! Compute the effect of interest
          * @param measurements a matrix storing the measured data for each subject in a column
          * @param design the design matrix (unlike other packages a column of ones is NOT automatically added for correlation analysis)
          * @param contrast a matrix defining the group difference
          * @param effect the matrix containing the output effect
          */
          template <typename ValueType>
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> abs_effect_size (const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& measurements,
                                                                                      const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design,
                                                                                      const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& contrast) {
              return contrast * solve_betas (measurements, design);
          }


          /*! Compute the pooled standard deviation
          * @param measurements a matrix storing the measured data for each subject in a column
          * @param design the design matrix (unlike other packages a column of ones is NOT automatically added for correlation analysis)
          * @param stdev the matrix containing the output standard deviation size
          */
          template <typename ValueType>
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> stdev (const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& measurements,
                                                                            const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design) {
              Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> residuals = measurements.transpose() - design * solve_betas (measurements, design); //TODO
              residuals = residuals.array().pow(2.0);
              Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> one_over_dof (1, measurements.cols());  //TODO supply transposed measurements
              one_over_dof.fill(1.0 / ValueType(design.rows()-Math::rank(design)));
              return (one_over_dof * residuals).array().sqrt();
          }


          /*! Compute cohen's d, the standardised effect size between two means
          * @param measurements a matrix storing the measured data for each subject in a column
          * @param design the design matrix (unlike other packages a column of ones is NOT automatically added for correlation analysis)
          * @param contrast a matrix defining the group difference
          * @param cohens_d the matrix containing the output standardised effect size
          */
          template <typename ValueType>
            Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic> std_effect_size (const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& measurements,
                                                                                      const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& design,
                                                                                      const Eigen::Matrix<ValueType, Eigen::Dynamic, Eigen::Dynamic>& contrast) {
              return abs_effect_size (measurements, design, contrast).array() / stdev (measurements, design).array();
          }
          //! @}
      }

      /** \addtogroup Statistics
      @{ */
      /*! A class to compute t-statistics using a General Linear Model. */
      class GLMTTest
      {
        public:
          /*!
          * @param measurements a matrix storing the measured data for each subject in a column //TODO
          * @param design the design matrix (unlike other packages a column of ones is NOT automatically added for correlation analysis)
          * @param contrast a matrix containing the contrast of interest.
          */
          GLMTTest (const Eigen::MatrixXf& measurements,
                    const Eigen::MatrixXf& design,
                    const Eigen::MatrixXf& contrast) :
            y (measurements),
            X (design),
            scaled_contrasts (GLM::scale_contrasts (contrast, X, X.rows()-rank(X)).transpose())
          {
            pinvX = Math::pinv (X.cast<double>()).template cast<float>();
          }

          /*! Compute the t-statistics
          * @param perm_labelling a vector to shuffle the rows in the design matrix (for permutation testing)
          * @param stats the vector containing the output t-statistics
          * @param max_stat the maximum t-statistic
          * @param min_stat the minimum t-statistic
          */
          void operator() (const std::vector<size_t>& perm_labelling, std::vector<float>& stats,
                           float& max_stat, float& min_stat) const
          {
            stats.resize (y.rows(), 0.0);
            Eigen::MatrixXf tvalues, betas, residuals, SX, pinvSX;

            SX.resize (X.rows(), X.cols());
            pinvSX.resize (pinvX.rows(), pinvX.cols());
            for (ssize_t i = 0; i < X.rows(); ++i) {
              // TODO: check whether we should permute rows or columns
              SX.row(i) = X.row (perm_labelling[i]);
              pinvSX.col(i) = pinvX.col (perm_labelling[i]);
            }

            pinvSX.transposeInPlace();
            SX.transposeInPlace();
            for (ssize_t i = 0; i < y.rows(); i += GLM_BATCH_SIZE) {
              Eigen::MatrixXf tmp = y.block(i, 0, std::min (i+GLM_BATCH_SIZE, y.rows()), y.cols());
              GLM::ttest (tvalues, SX, pinvSX, tmp, scaled_contrasts, betas, residuals);
              for (ssize_t n = 0; n < tvalues.rows(); ++n) {
                float val = tvalues(n,0);
                if (std::isfinite (val)) {
                  if (val > max_stat)
                    max_stat = val;
                  if (val < min_stat)
                    min_stat = val;
                } else {
                  val = float(0.0);
                }
                stats[i+n] = val;
              }
            }
          }

          size_t num_subjects () const { return y.cols(); }
          size_t num_elements () const { return y.rows(); }

        protected:
          const Eigen::MatrixXf& y;
          Eigen::MatrixXf X, pinvX, scaled_contrasts;
      };
      //! @}

    }
  }
}


#endif
