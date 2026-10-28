/*
 * SPDX-FileCopyrightText: 2026 EsyProject contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include <float.h>
#include <math.h>
#include <string.h>

#include "esy_common/defs/base/esy_common_defs.h"
#include "esy_middleware/numerics/esy_fit.h"
#include "esy_middleware/numerics/esy_linalg.h"
#include "esy_middleware/numerics/esy_poly_fit.h"

void setUp(void) {}

void tearDown(void) {}

static void linear_feature(float x, float *phi, int m, void *user) {
  (void)user;

  if (m > 0) {
    phi[0] = 1.0f;
  }
  if (m > 1) {
    phi[1] = x;
  }
}

static void constant_feature(float x, float *phi, int m, void *user) {
  (void)x;
  (void)user;

  if (m > 0) {
    phi[0] = 1.0f;
  }
}

static void quadratic_feature(float x, float *phi, int m, void *user) {
  (void)user;

  if (m > 0) {
    phi[0] = 1.0f;
  }
  if (m > 1) {
    phi[1] = x;
  }
  if (m > 2) {
    phi[2] = x * x;
  }
}

void test_linalg_solve_solves_spd_system(void) {
  float A[] = {
      4.0f, 1.0f,
      1.0f, 3.0f,
  };
  float b[] = {1.0f, 2.0f};
  float x[] = {0.0f, 0.0f};

  TEST_ASSERT_EQUAL_INT(0, esy_linalg_solve(A, b, x, 2));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.090909f, x[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.636364f, x[1]);
}

void test_linalg_solve_rejects_invalid_or_non_spd_inputs(void) {
  float A[] = {
      1.0f, 2.0f,
      2.0f, 1.0f,
  };
  float b[] = {1.0f, 2.0f};
  float x[] = {0.0f, 0.0f};

  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(NULL, b, x, 2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A, NULL, x, 2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A, b, NULL, 2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A, b, x, 0));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A, b, x, 2));
}

void test_linalg_inverse_inverts_small_matrix(void) {
  const float A[] = {
      4.0f, 7.0f,
      2.0f, 6.0f,
  };
  float inv[4] = {0.0f};

  TEST_ASSERT_EQUAL_INT(0, esy_linalg_inverse(A, inv, 2));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.6f, inv[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.7f, inv[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -0.2f, inv[2]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.4f, inv[3]);
}

void test_linalg_inverse_rejects_invalid_or_singular_inputs(void) {
  const float singular[] = {
      1.0f, 2.0f,
      2.0f, 4.0f,
  };
  float inv[4] = {0.0f};

  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(NULL, inv, 2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(singular, NULL, 2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(singular, inv, 0));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(singular, inv, 2));
}

void test_fit_train_recovers_linear_model(void) {
  const float X[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const float Y[] = {-1.0f, 1.0f, 3.0f, 5.0f, 7.0f};
  esy_fit_model_t model;

  TEST_ASSERT_EQUAL_INT(
      ESY_OK,
      esy_fit_train(X, Y, 5, 2, 0.0f, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(2, model.m);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.0f, model.w[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, model.w[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, model.rmse_train);
}

void test_fit_train_rejects_invalid_arguments(void) {
  const float X[] = {0.0f, 1.0f};
  const float Y[] = {1.0f, 2.0f};
  esy_fit_model_t model;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_train(NULL, Y, 2, 2, 0.0f, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_train(X, NULL, 2, 2, 0.0f, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_train(X, Y, 2, 2, 0.0f, NULL, NULL, &model));
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_train(X, Y, 1, 2, 0.0f, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_train(X, Y, 2, 2, -1.0f, linear_feature, NULL, &model));
}

void test_fit_train_uses_ridge_to_solve_repeated_samples(void) {
  const float X[] = {1.0f, 1.0f, 1.0f};
  const float Y[] = {4.0f, 4.0f, 4.0f};
  esy_fit_model_t model;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_FAIL,
      esy_fit_train(X, Y, 3, 2, 0.0f, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(
      ESY_OK,
      esy_fit_train(X, Y, 3, 2, 0.1f, linear_feature, NULL, &model));
  TEST_ASSERT_LESS_THAN_FLOAT(0.2f, model.rmse_train);
}

void test_fit_select_m_loocv_selects_lower_dimension_on_linear_data(void) {
  const float X[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const float Y[] = {-1.0f, 1.0f, 3.0f, 5.0f, 7.0f};
  float cv = -1.0f;
  int best_m;

  best_m = esy_fit_select_m_loocv(
      X, Y, 5, 1, 2, 0.001f, linear_feature, NULL, &cv);
  TEST_ASSERT_EQUAL_INT(2, best_m);
  TEST_ASSERT_GREATER_OR_EQUAL_FLOAT(0.0f, cv);
}

void test_fit_select_m_loocv_clears_output_and_rejects_invalid_arguments(void) {
  const float X[] = {0.0f, 1.0f};
  const float Y[] = {1.0f, 2.0f};
  float cv = 99.0f;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_select_m_loocv(NULL, Y, 2, 1, 1, 0.0f,
                             constant_feature, NULL, &cv));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, cv);

  cv = 99.0f;
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG,
      esy_fit_select_m_loocv(X, Y, 1, 1, 1, 0.0f,
                             constant_feature, NULL, &cv));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, cv);
}

void test_fit_select_m_loocv_returns_fail_when_all_candidates_are_singular(void) {
  const float X[] = {1.0f, 1.0f, 1.0f};
  const float Y[] = {4.0f, 4.0f, 4.0f};
  float cv = 99.0f;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_FAIL,
      esy_fit_select_m_loocv(X, Y, 3, 2, 2, 0.0f,
                             linear_feature, NULL, &cv));
}

void test_fit_train_matches_golden_quadratic_feature_weights(void) {
  const float X[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const float Y[] = {2.0f, 1.25f, 1.0f, 1.25f, 2.0f};
  esy_fit_model_t model;

  TEST_ASSERT_EQUAL_INT(
      ESY_OK,
      esy_fit_train(X, Y, 5, 3, 0.0f, quadratic_feature, NULL, &model));
  TEST_ASSERT_EQUAL_INT(3, model.m);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, model.w[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, model.w[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, model.w[2]);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, model.rmse_train);
}

void test_poly_fit_recovers_quadratic_coefficients(void) {
  const float X[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const float Y[] = {0.0f, 0.5f, 2.0f, 4.5f, 8.0f};
  float A[3] = {0.0f};
  esy_poly_model_t model;
  float rmse = -1.0f;

  TEST_ASSERT_EQUAL_INT(
      ESY_OK, esy_poly_fit(X, Y, 5, 2, 0.0f, A, &model, &rmse));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 2.0f, A[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 2.0f, A[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.5f, A[2]);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.0f, rmse);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 4.5f,
                           esy_poly_predict_raw(A, 2, 1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 4.5f,
                           esy_poly_predict_scaled(&model, 1.0f));
}

void test_poly_fit_rejects_invalid_arguments_and_preserves_rmse_output(void) {
  const float X[] = {0.0f, 1.0f};
  const float Y[] = {1.0f, 2.0f};
  float A[2] = {0.0f};
  float rmse = 12.0f;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG, esy_poly_fit(NULL, Y, 2, 1, 0.0f, A, NULL, &rmse));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, rmse);
  rmse = 12.0f;
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG, esy_poly_fit(X, Y, 1, 1, 0.0f, A, NULL, &rmse));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, rmse);
  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_INVALID_ARG, esy_poly_fit(X, Y, 2, 1, -1.0f, A, NULL, NULL));
}

void test_poly_fit_handles_constant_x_with_ridge(void) {
  const float X[] = {5.0f, 5.0f, 5.0f};
  const float Y[] = {3.0f, 3.0f, 3.0f};
  float A[2] = {0.0f};
  esy_poly_model_t model;
  float rmse = -1.0f;

  TEST_ASSERT_EQUAL_INT(
      ESY_ERR_FAIL, esy_poly_fit(X, Y, 3, 1, 0.0f, A, &model, &rmse));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, rmse);

  TEST_ASSERT_EQUAL_INT(
      ESY_OK, esy_poly_fit(X, Y, 3, 1, 0.1f, A, &model, &rmse));
  TEST_ASSERT_LESS_THAN_FLOAT(0.2f, rmse);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 3.0f, esy_poly_predict_scaled(&model, 5.0f));
}

void test_poly_fit_uses_lower_degree_for_constant_data_tie(void) {
  const float X[] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
  const float Y[] = {7.0f, 7.0f, 7.0f, 7.0f, 7.0f};

  TEST_ASSERT_EQUAL_INT(0, esy_poly_select_degree_loocv(X, Y, 5, 0, 2, 0.001f));
}

void test_poly_select_degree_loocv_prefers_quadratic_for_quadratic_data(void) {
  const float X[] = {-3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f};
  const float Y[] = {5.5f, 1.0f, -0.5f, 1.0f, 5.5f, 13.0f, 23.5f};

  TEST_ASSERT_EQUAL_INT(2, esy_poly_select_degree_loocv(X, Y, 7, 0, 2, 0.001f));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
                        esy_poly_select_degree_loocv(NULL, Y, 7, 0, 2, 0.0f));
}

static void nonfinite_feature(float x, float *phi, int m, void *user) {
  (void)x; (void)user;
  for (int j = 0; j < m; ++j) phi[j] = NAN;
}

void test_fit_loocv_rejects_interpolating_ols_and_matches_ridge_reference(void) {
  const float X[] = {0, 1}, Y[] = {0, 1};
  float cv;
  TEST_ASSERT_EQUAL_INT(1, esy_fit_select_m_loocv(X, Y, 2, 1, 2, 0,
                                              linear_feature, NULL, &cv));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1, cv);
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_fit_select_m_loocv(X, Y, 2, 2, 2, 0,
                                                         linear_feature, NULL, NULL));
  /* Fold (x=1,y=1) has both weights 1/(2+lambda); the other fold has zero weights. */
  const double intercept = 1.0 / (2.0 + 1e-4);
  const float expected = (float)sqrt((intercept * intercept + 1.0) / 2.0);
  TEST_ASSERT_EQUAL_INT(2, esy_fit_select_m_loocv(X, Y, 2, 2, 2, 1e-4f,
                                              linear_feature, NULL, &cv));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, expected, cv);
}

void test_fit_loocv_matches_explicit_public_training_on_noisy_data(void) {
  const float X[] = {-2, -1, 0, 1, 2}, Y[] = {-0.9f, 1.2f, 2.9f, 5.2f, 6.8f};
  for (int m = 1; m <= 3; ++m) {
    double se = 0;
    for (int held = 0; held < 5; ++held) {
      float tx[4], ty[4]; int k = 0;
      for (int i = 0; i < 5; ++i) {
        if (i != held) {tx[k] = X[i]; ty[k++] = Y[i];}
      }
      esy_fit_model_t model;
      TEST_ASSERT_EQUAL_INT(ESY_OK,
          esy_fit_train(tx, ty, 4, m, 0.1f, quadratic_feature, NULL, &model));
      float phi[3]; quadratic_feature(X[held], phi, m, NULL);
      double prediction = 0;
      for (int j = 0; j < m; ++j) prediction += phi[j] * model.w[j];
      double residual = Y[held] - prediction; se += residual * residual;
    }
    float cv;
    TEST_ASSERT_EQUAL_INT(m, esy_fit_select_m_loocv(X, Y, 5, m, m, 0.1f,
                                                quadratic_feature, NULL, &cv));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)sqrt(se / 5), cv);
  }
}

void test_fit_failure_preserves_model_and_allows_retry(void) {
  float X[] = {0, 1}, Y[] = {0, 1};
  esy_fit_model_t model;
  memset(&model, 0xA5, sizeof(model));
  unsigned char original[sizeof(model)]; memcpy(original, &model, sizeof(model));
  const float invalid[] = {NAN, INFINITY, -INFINITY};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_fit_train(X, Y, 2, 2, invalid[i], linear_feature, NULL, &model));
    X[1] = invalid[i];
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_fit_train(X, Y, 2, 2, 0, linear_feature, NULL, &model));
    X[1] = 1; Y[1] = invalid[i];
    TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,
        esy_fit_train(X, Y, 2, 2, 0, linear_feature, NULL, &model));
    Y[1] = 1;
  }
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL,
      esy_fit_train(X, Y, 2, 2, 0, nonfinite_feature, NULL, &model));
  X[0] = 1e20f; X[1] = 2e20f;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL,
      esy_fit_train(X, Y, 2, 2, 0, linear_feature, NULL, &model));
  TEST_ASSERT_EQUAL_MEMORY(original, &model, sizeof(model));
  X[0] = 0; X[1] = 1;
  TEST_ASSERT_EQUAL_INT(ESY_OK,
      esy_fit_train(X, Y, 2, 2, 0, linear_feature, NULL, &model));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1, model.w[1]);
}

void test_fit_validates_dimensions_and_selection_inputs(void) {
  float X[] = {0, 1}, Y[] = {0, 1}; esy_fit_model_t model; float cv = 7;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,2,2,0,linear_feature,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,0,1,0,linear_feature,NULL,&model));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,ESY_FIT_MAX_N+1,1,0,linear_feature,NULL,&model));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,2,0,0,linear_feature,NULL,&model));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,2,ESY_FIT_MAX_M+1,0,linear_feature,NULL,&model));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_train(X,Y,1,2,1,linear_feature,NULL,&model));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,NULL,2,1,1,0,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,1,1,0,NULL,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,ESY_FIT_MAX_N+1,1,1,0,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,0,1,0,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,1,ESY_FIT_MAX_M+1,0,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,2,1,0,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,1,1,NAN,linear_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,1,1,-1,linear_feature,NULL,&cv));
  X[1] = NAN;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG, esy_fit_select_m_loocv(X,Y,2,1,1,0,linear_feature,NULL,&cv));
  X[1] = 1;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_fit_select_m_loocv(X,Y,2,1,1,0,nonfinite_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_fit_select_m_loocv(X,Y,2,3,3,1,quadratic_feature,NULL,&cv));
  TEST_ASSERT_EQUAL_FLOAT(0, cv);
}

void test_linalg_rejects_nonfinite_inputs_and_output_overflow(void) {
  float A[4] = {1,0,0,1}, b[2] = {1,1}, out[4] = {123,123,123,123};
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A,b,out,ESY_LINALG_MAX_M+1));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(A,out,ESY_LINALG_MAX_M+1));
  A[0] = NAN;
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A,b,out,2));
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(A,out,2));
  TEST_ASSERT_EQUAL_FLOAT(123, out[0]);
  A[0] = 1; b[0] = INFINITY;
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A,b,out,2));
  A[0] = 1e-10f; b[0] = FLT_MAX;
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A,b,out,1));
  A[0] = 1e-10f; b[0] = 1e30f;
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_solve(A,b,out,1));
  const float overflow[] = {1e-10f, FLT_MAX, 0, 1e-10f};
  TEST_ASSERT_NOT_EQUAL(0, esy_linalg_inverse(overflow,out,2));
  const float pivot[] = {0, 1, 1, 0};
  TEST_ASSERT_EQUAL_INT(0, esy_linalg_inverse(pivot,out,2));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f,1,out[1]);
}

void test_poly_numerical_failure_is_transactional_and_large_scaling_is_finite(void) {
  float X[] = {0,1e-20f,2e-20f}, Y[] = {0,1,4}, A[] = {123,456,789};
  esy_poly_model_t model; memset(&model,0xA5,sizeof(model));
  unsigned char before[sizeof(model)]; memcpy(before,&model,sizeof(model));
  float rmse = 42;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_poly_fit(X,Y,3,2,0,A,&model,&rmse));
  TEST_ASSERT_EQUAL_FLOAT(123,A[0]); TEST_ASSERT_EQUAL_FLOAT(789,A[2]);
  TEST_ASSERT_EQUAL_MEMORY(before,&model,sizeof(model)); TEST_ASSERT_EQUAL_FLOAT(0,rmse);
  X[0]=2e38f;X[1]=3e38f;Y[0]=2;Y[1]=3;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_poly_fit(X,Y,2,1,0,A,&model,&rmse));
  TEST_ASSERT_TRUE(isfinite(model.mean));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f,2,esy_poly_predict_scaled(&model,X[0]));
  X[0]=-FLT_MAX;X[1]=FLT_MAX;Y[0]=-1;Y[1]=1;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_poly_fit(X,Y,2,1,0,A,&model,&rmse));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f,1,esy_poly_predict_scaled(&model,X[1]));
  X[0]=0;X[1]=FLT_TRUE_MIN;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_poly_fit(X,Y,2,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL, esy_poly_select_degree_loocv(X,Y,2,0,1,1));
}

void test_poly_validates_all_public_bounds_and_nonfinite_samples(void) {
  float X[]={0,1},Y[]={0,1},A[2];
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,NULL,2,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,1,0,NULL,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,0,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,ESY_FIT_MAX_N+1,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,-1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,ESY_FIT_MAX_DEG+1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,1,NAN,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,1,1,1,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,NULL,2,0,1,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,1,0,1,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,ESY_FIT_MAX_N+1,0,1,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,-1,1,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,0,ESY_FIT_MAX_DEG+1,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,1,0,0));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,0,1,-1));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,0,1,NAN));
  X[1]=NAN;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,0,1,0));
  X[1]=1;Y[1]=INFINITY;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_fit(X,Y,2,1,0,A,NULL,NULL));
  TEST_ASSERT_EQUAL_INT(ESY_ERR_INVALID_ARG,esy_poly_select_degree_loocv(X,Y,2,0,1,0));
  Y[1]=1;
  TEST_ASSERT_EQUAL_INT(ESY_ERR_FAIL,esy_poly_select_degree_loocv(X,Y,2,1,1,0));
  TEST_ASSERT_EQUAL_INT(ESY_OK,esy_poly_fit(X,Y,2,1,0,A,NULL,NULL));
}

static void indexed_feature(float x, float *phi, int m, void *user) {
  (void)user;
  for (int j = 0; j < m; ++j) phi[j] = x == (float)j ? 1.0f : 0.0f;
}

void test_default_capacity_boundaries_fit_select_and_predict(void) {
  float X[ESY_FIT_MAX_N], Y[ESY_FIT_MAX_N];
  esy_fit_model_t fit;
  for (int i = 0; i < ESY_FIT_MAX_N; ++i) {
    X[i] = (float)(i % ESY_FIT_MAX_M);
    Y[i] = X[i] + 0.25f;
  }
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_fit_train(X, Y, ESY_FIT_MAX_N, ESY_FIT_MAX_M,
                                            0, indexed_feature, NULL, &fit));
  for (int j = 0; j < ESY_FIT_MAX_M; ++j) {
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, (float)j + 0.25f, fit.w[j]);
  }
  float cv;
  TEST_ASSERT_EQUAL_INT(ESY_FIT_MAX_M,
      esy_fit_select_m_loocv(X, Y, ESY_FIT_MAX_N, ESY_FIT_MAX_M, ESY_FIT_MAX_M,
                             0, indexed_feature, NULL, &cv));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0, cv);
  for (int i = 0; i < ESY_FIT_MAX_N; ++i) {
    X[i] = -1.0f + 2.0f * i / (ESY_FIT_MAX_N - 1);
    Y[i] = 2.0f + powf(X[i], ESY_FIT_MAX_DEG);
  }
  float A[ESY_FIT_MAX_DEG + 1]; esy_poly_model_t poly;
  TEST_ASSERT_EQUAL_INT(ESY_OK, esy_poly_fit(X, Y, ESY_FIT_MAX_N,
      ESY_FIT_MAX_DEG, 0, A, &poly, NULL));
  for (int i = 0; i < ESY_FIT_MAX_N; ++i) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, Y[i], esy_poly_predict_scaled(&poly, X[i]));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, Y[i], esy_poly_predict_raw(A, ESY_FIT_MAX_DEG, X[i]));
  }
}

void test_linalg_maximum_dimension_identity(void) {
  float A[ESY_LINALG_MAX_M * ESY_LINALG_MAX_M] = {0};
  float inv[ESY_LINALG_MAX_M * ESY_LINALG_MAX_M];
  float b[ESY_LINALG_MAX_M], x[ESY_LINALG_MAX_M];
  for (int i = 0; i < ESY_LINALG_MAX_M; ++i) {
    A[i * ESY_LINALG_MAX_M + i] = 1;
    b[i] = (float)i;
  }
  TEST_ASSERT_EQUAL_INT(0, esy_linalg_inverse(A, inv, ESY_LINALG_MAX_M));
  TEST_ASSERT_EQUAL_FLOAT_ARRAY(A, inv, ESY_LINALG_MAX_M * ESY_LINALG_MAX_M);
  TEST_ASSERT_EQUAL_INT(0, esy_linalg_solve(A, b, x, ESY_LINALG_MAX_M));
  TEST_ASSERT_EQUAL_FLOAT_ARRAY(b, x, ESY_LINALG_MAX_M);
}
