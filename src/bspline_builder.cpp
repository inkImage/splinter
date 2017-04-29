/*
 * This file is part of the SPLINTER library.
 * Copyright (C) 2012 Bjarne Grimstad (bjarne.grimstad@gmail.com).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include "bspline_builder.h"
#include <linear_solvers.h>
#include <serializer.h>
#include <iostream>
#include <utilities.h>

namespace SPLINTER
{
// Default constructor
BSpline::Builder::Builder(unsigned int dim_x, unsigned int dim_y)
        : _dim_x(dim_x),
          _dim_y(dim_y),
          _degrees(std::vector<unsigned int>(_dim_x, 3)),
          _numBasisFunctions(std::vector<unsigned int>(_dim_x, 1)),
          _knotSpacing(KnotSpacing::AS_SAMPLED)
{
}

/**
 * Fit B-spline to data
 */
BSpline BSpline::Builder::fit(const DataTable &data,
                              Smoothing smoothing,
                              double alpha,
                              std::vector<double> weights) const
{
    if (data.getDimX() != _dim_x)
        throw Exception("BSpline::Builder::fit: Expected " + std::to_string(_dim_x) + " input variables.");

    if (data.getDimY() != _dim_y)
        throw Exception("BSpline::Builder::fit: Expected " + std::to_string(_dim_y) + " output variables.");

    if (alpha < 0)
        throw Exception("BSpline::Builder::fit: alpha must be non-negative.");

    if (weights.size() > 0 && data.getNumSamples() != weights.size()) {
        throw Exception("BSpline::Builder::fit: number of weights must equal number of data points.");
    }

#ifndef NDEBUG
    if (!data.isGridComplete())
        std::cout << "BSpline::Builder::fit: Building B-spline from irregular (incomplete) grid." << std::endl;
#endif // NDEBUG

    // Build knot vectors
    auto knotVectors = computeKnotVectors(data, _degrees, _numBasisFunctions, _knotSpacing);

    // Build B-spline (with default coefficients)
    auto bspline = BSpline(_dim_x, _dim_y, knotVectors, _degrees);

    // Compute coefficients from samples and update B-spline
    auto coefficients = computeControlPoints(bspline, data, smoothing, alpha, weights);
    bspline.setControlPoints(coefficients);

    return bspline;
}

/**
 * Find coefficients of B-spline by solving:
 * min ||W*X*C - W*Y||^2 + alpha*||R||^2,
 * where
 * X = (m x n) matrix of n basis functions evaluated at m sample points,
 * Y = vector of m sample points y-values (or x-values when calculating knot averages),
 * C = B-spline coefficients (or knot averages),
 * R = Regularization matrix (n x n),
 * alpha = regularization parameter,
 * W = Diagonal weight matrix (m x m).
 *
 * The optimal control point matrix C is the solution of the linear system of equations:
 * (X'*W*X + alpha*R) C = X'*W*Y
 */
DenseMatrix BSpline::Builder::computeControlPoints(const BSpline &bspline,
                                                   const DataTable &data,
                                                   Smoothing smoothing,
                                                   double alpha,
                                                   std::vector<double> weights) const
{
    unsigned int num_samples = data.getNumSamples();
    unsigned int num_basis_functions = bspline.getNumBasisFunctions();
    SparseMatrix X = computeBasisFunctionMatrix(bspline, data);
    SparseMatrix Xt = X.transpose();
    DenseMatrix Y = stackSamplePointValues(data);

    // Regularization matrix
    SparseMatrix R(num_basis_functions, num_basis_functions);

    if (smoothing == Smoothing::IDENTITY) {
        /*
         * Tikhonov regularization (or ridge regression) with the Identity matrix
         * See: https://en.wikipedia.org/wiki/Tikhonov_regularization
         */
        auto I = SparseMatrix(num_basis_functions, num_basis_functions);
        I.setIdentity();
        R = I;
    }
    else if (smoothing == Smoothing::PSPLINE)
    {
        /*
         * The P-Spline is a smoothing B-spline which relaxes the interpolation constraints on the control points to allow
         * smoother spline curves. It minimizes an objective which penalizes both deviation from sample points (to lower bias)
         * and the magnitude of second derivatives (to lower variance).
         *
         * Regularization matrix is given as R = D'*D, where D is the second-order finite difference matrix
         */

        // Second order finite difference matrix
        SparseMatrix D = getSecondOrderFiniteDifferenceMatrix(bspline);
        R = D.transpose()*D;
    }

    // Weight matrix
    // NOTE: Consider using Eigen::DiagonalMatrix<double, num_samples> W()
    SparseMatrix W(num_samples, num_samples);

    if (weights.size() > 0) {
        W = computeWeightMatrix(weights);
    } else {
        W.setIdentity();
    }

    // Left-hand side matrix
    // NOTE2: consider changing regularization factor to (alpha/numSample)
    SparseMatrix A = Xt*W*X;

    if (smoothing != Smoothing::NONE) {
        A += alpha*R;
    }

    // Compute right-hand side matrices
    DenseMatrix B = Xt*W*Y;

    // Solve equation AC = B for control points C
    DenseMatrix C;

    int num_equations = A.rows();
    int max_num_equations = 100;
    bool solve_as_dense = (num_equations < max_num_equations);

    if (!solve_as_dense)
    {
        #ifndef NDEBUG
        std::cout << "BSpline::Builder::computeBSplineCoefficients: Computing B-spline control points using sparse solver." << std::endl;
        #endif // NDEBUG

        SparseLU<DenseMatrix> s;

        solve_as_dense = !s.solve(A, B, C);
    }

    if (solve_as_dense)
    {
        #ifndef NDEBUG
        std::cout << "BSpline::Builder::computeBSplineCoefficients: Computing B-spline control points using dense solver." << std::endl;
        #endif // NDEBUG

        DenseMatrix Ad = A.toDense();
        DenseQR<DenseMatrix> s;
        // DenseSVD<DenseVector> s;
        if (!s.solve(Ad, B, C))
        {
            throw Exception("BSpline::Builder::computeBSplineCoefficients: Failed to solve for B-spline coefficients.");
        }
    }

    return C;
}

SparseMatrix BSpline::Builder::computeBasisFunctionMatrix(const BSpline &bspline, const DataTable &data) const
{
    unsigned int num_samples = data.getNumSamples();

    // TODO: Reserve nnz per row (degree+1)
    //int nnzPrCol = bspline.basis.numSupported();

    SparseMatrix A(num_samples, bspline.getNumBasisFunctions());
    //A.reserve(DenseVector::Constant(num_samples, nnzPrCol)); // TODO: should reserve nnz per row!

    int i = 0;
    for (auto it = data.cbegin(); it != data.cend(); ++it, ++i)
    {
        DenseVector xi = stdToEigVec(it->getX());
        SparseVector basis_values = bspline.evalBasis(xi);
        for (SparseVector::InnerIterator it2(basis_values); it2; ++it2)
            A.insert(i, it2.index()) = it2.value();
    }

    A.makeCompressed();

    return A;
}

DenseMatrix BSpline::Builder::stackSamplePointValues(const DataTable &data) const
{
    DenseMatrix B = DenseMatrix::Zero(data.getNumSamples(), data.getDimY());

    int i = 0;
    for (auto it = data.cbegin(); it != data.cend(); ++it, ++i)
    {
        auto y = it->getY();
        for (unsigned int j = 0; j < data.getDimY(); ++j)
            B(i, j) = y.at(j);
    }
    return B;
}

/**
 * Function for generating second order finite-difference matrix, which is used for penalizing the
 * (approximate) second derivative in control point calculation for P-splines.
 */
SparseMatrix BSpline::Builder::getSecondOrderFiniteDifferenceMatrix(const BSpline &bspline) const
{
    unsigned int num_variables = bspline.getDimX();

    // Number of (total) basis functions - defines the number of columns in D
    unsigned int num_cols = bspline.getNumBasisFunctions();
    std::vector<unsigned int> num_basis_functions = bspline.getNumBasisFunctionsPerVariable();

    // Number of basis functions (and coefficients) in each variable
    std::vector<unsigned int> dims;
    for (unsigned int i = 0; i < num_variables; i++)
        dims.push_back(num_basis_functions.at(i));

    std::reverse(dims.begin(), dims.end());

    for (unsigned int i=0; i < num_variables; ++i)
        if (num_basis_functions.at(i) < 3)
            throw Exception("BSpline::Builder::getSecondOrderDifferenceMatrix: Need at least three coefficients/basis function per variable.");

    // Number of rows in D and in each block
    int num_rows = 0;
    std::vector< int > num_blk_rows;
    for (unsigned int i = 0; i < num_variables; i++)
    {
        int prod = 1;
        for (unsigned int j = 0; j < num_variables; j++)
        {
            if (i == j)
                prod *= (dims[j] - 2);
            else
                prod *= dims[j];
        }
        num_rows += prod;
        num_blk_rows.push_back(prod);
    }

    // Resize and initialize D
    SparseMatrix D(num_rows, num_cols);
    D.reserve(DenseVector::Constant(num_cols, 2*num_variables));   // D has no more than two elems per col per dim

    int i = 0; // Row index
    // Loop though each dimension (each dimension has its own block)
    for (unsigned int d = 0; d < num_variables; d++)
    {
        // Calculate left and right products
        int left_prod = 1;
        int right_prod = 1;
        for (unsigned int k = 0; k < d; k++)
        {
            left_prod *= dims[k];
        }
        for (unsigned int k = d+1; k < num_variables; k++)
        {
            right_prod *= dims[k];
        }

        // Loop through subblocks on the block diagonal
        for (int j = 0; j < right_prod; j++)
        {
            // Start column of current subblock
            int blkBaseCol = j*left_prod*dims[d];
            // Block rows [I -2I I] of subblock
            for (unsigned int l = 0; l < (dims[d] - 2); l++)
            {
                // Special case for first dimension
                if (d == 0)
                {
                    int k = j*left_prod*dims[d] + l;
                    D.insert(i,k) = 1;
                    k += left_prod;
                    D.insert(i,k) = -2;
                    k += left_prod;
                    D.insert(i,k) = 1;
                    i++;
                }
                else
                {
                    // Loop for identity matrix
                    for (int n = 0; n < left_prod; n++)
                    {
                        int k = blkBaseCol + l*left_prod + n;
                        D.insert(i,k) = 1;
                        k += left_prod;
                        D.insert(i,k) = -2;
                        k += left_prod;
                        D.insert(i,k) = 1;
                        i++;
                    }
                }
            }
        }
    }

    D.makeCompressed();

    return D;
}

SparseMatrix BSpline::Builder::computeWeightMatrix(const std::vector<double> weights) const
{
    // TODO: use DiagonalMatrix here
    auto eig_weights = stdToEigVec(weights);
    DenseMatrix D = eig_weights.asDiagonal();
    return D.sparseView();
}

} // namespace SPLINTER