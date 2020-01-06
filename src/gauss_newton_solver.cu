#pragma once 


#include "gauss_newton_solver.h"
#include "util.h"
#include "device_util.h"
#include "device_array.h"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"

__global__ void cuComputeJacobianSparseFeatures(
	//shared memory
	const int nFeatures,
	const int nShapeCoeffs, const int nExpressionCoeffs,
	const int nUnknowns, const int nResiduals,
	const int nVerticesTimes3, const int nShapeCoeffsTotal, const int nExpressionCoeffsTotal,
	const float regularizationWeight,

	glm::mat4 face_pose, glm::mat3 drx, glm::mat3 dry, glm::mat3 drz, glm::mat4 projection, Eigen::Matrix3f jacobian_local,

	//device memory input
	int* prior_local_ids, glm::vec3* current_face, glm::vec2* sparse_features,
	float* p_shape_basis, float* p_expression_basis, float* p_coefficients_shape, float* p_coefficients_expression,

	//device memory output
	float* p_jacobian, float* p_residuals)
{
	int i = util::getThreadIndex1D();

	Eigen::Map<Eigen::MatrixXf> jacobian(p_jacobian, nResiduals, nUnknowns);
	Eigen::Map<Eigen::VectorXf> residuals(p_residuals, nResiduals);

	int offset_rows = nFeatures * 2;
	int offset_cols = 7;

	// Regularization terms
	if (i >= nFeatures)
	{
		const int current_index = i - nFeatures;
		const int shift = current_index >= nShapeCoeffs ? nShapeCoeffs : 0;

		offset_rows += shift;
		offset_cols += shift;

		const int relative_index = current_index - shift;

		const float coefficient = shift > 0 ? p_coefficients_expression[relative_index] : p_coefficients_shape[relative_index];

		auto sqrt_wreg = glm::sqrt(regularizationWeight);
		jacobian(offset_rows + relative_index, offset_cols + relative_index) = sqrt_wreg;
		residuals(offset_rows + relative_index) = coefficient * sqrt_wreg;

		return;
	}

	Eigen::Map<Eigen::MatrixXf> shape_basis(p_shape_basis, nVerticesTimes3, nShapeCoeffsTotal);
	Eigen::Map<Eigen::MatrixXf> expression_basis(p_expression_basis, nVerticesTimes3, nExpressionCoeffsTotal);

	Eigen::Matrix<float, 2, 3> jacobian_proj = Eigen::MatrixXf::Zero(2, 3);

	Eigen::Matrix<float, 3, 3> jacobian_world = Eigen::MatrixXf::Zero(3, 3);
	jacobian_world(1, 1) = projection[1][1];
	jacobian_world(2, 2) = -1.0f;

	Eigen::Matrix<float, 3, 1> jacobian_intrinsics = Eigen::MatrixXf::Zero(3, 1);

	Eigen::Matrix<float, 3, 6> jacobian_pose = Eigen::MatrixXf::Zero(3, 6);
	jacobian_pose(0, 3) = 1.0f;
	jacobian_pose(1, 4) = 1.0f;
	jacobian_pose(2, 5) = 1.0f;

	auto vertex_id = prior_local_ids[i];
	auto local_coord = current_face[vertex_id];

	auto world_coord = face_pose * glm::vec4(local_coord, 1.0f);
	auto proj_coord = projection * world_coord;
	auto uv = glm::vec2(proj_coord.x, proj_coord.y) / proj_coord.w;

	//Residual
	auto residual = uv - sparse_features[i];

	residuals(i * 2) = residual.x;
	residuals(i * 2 + 1) = residual.y;

	//Jacobian for homogenization (AKA division by w)
	auto one_over_wp = 1.0f / proj_coord.w;
	jacobian_proj(0, 0) = one_over_wp;
	jacobian_proj(0, 2) = -proj_coord.x * one_over_wp * one_over_wp;

	jacobian_proj(1, 1) = one_over_wp;
	jacobian_proj(1, 2) = -proj_coord.y * one_over_wp * one_over_wp;

	//Jacobian for projection
	jacobian_world(0, 0) = projection[0][0];

	//Jacobian for intrinsics
	jacobian_intrinsics(0, 0) = world_coord.x;
	jacobian.block<2, 1>(i * 2, 0) = jacobian_proj * jacobian_intrinsics;

	//Derivative of world coordinates with respect to rotation coefficients
	auto dx = drx * local_coord;
	auto dy = dry * local_coord;
	auto dz = drz * local_coord;

	jacobian_pose(0, 0) = dx[0];
	jacobian_pose(1, 0) = dx[1];
	jacobian_pose(2, 0) = dx[2];
	jacobian_pose(0, 1) = dy[0];
	jacobian_pose(1, 1) = dy[1];
	jacobian_pose(2, 1) = dy[2];
	jacobian_pose(0, 2) = dz[0];
	jacobian_pose(1, 2) = dz[1];
	jacobian_pose(2, 2) = dz[2];

	auto jacobian_proj_world = jacobian_proj * jacobian_world;
	jacobian.block<2, 6>(i * 2, 1) = jacobian_proj_world * jacobian_pose;

	//Derivative of world coordinates with respect to local coordinates.
	//This is basically the rotation matrix.
	auto jacobian_proj_world_local = jacobian_proj_world * jacobian_local;

	//Derivative of local coordinates with respect to shape and expression parameters
	//This is basically the corresponding (to unique vertices we have chosen) rows of basis matrices.
	auto jacobian_shape = jacobian_proj_world_local * shape_basis.block(3 * vertex_id, 0, 3, nShapeCoeffs);
	jacobian.block(i * 2, 7, 2, nShapeCoeffs) = jacobian_shape;

	auto jacobian_expression = jacobian_proj_world_local * expression_basis.block(3 * vertex_id, 0, 3, nExpressionCoeffs);
	jacobian.block(i * 2, 7 + nShapeCoeffs, 2, nExpressionCoeffs) = jacobian_expression;
}

void GaussNewtonSolver::computeJacobianSparseFeatures(
	//shared memory
	const int nFeatures,
	const int nShapeCoeffs, const int nExpressionCoeffs,
	const int nUnknowns, const int nResiduals,
	const int nVerticesTimes3, const int nShapeCoeffsTotal, const int nExpressionCoeffsTotal,
	const float regularizationWeight,

	const glm::mat4& face_pose, const glm::mat3& drx, const glm::mat3& dry, const glm::mat3& drz, const glm::mat4& projection, const Eigen::Matrix3f& jacobian_local,

	//device memory input
	int* prior_local_ids, glm::vec3* current_face, glm::vec2* sparse_features,
	float* p_shape_basis, float* p_expression_basis, float* p_coefficients_shape, float* p_coefficients_expression,

	//device memory output
	float* p_jacobian, float* p_residuals
) const
{
	const int threads = nFeatures + m_params.num_shape_coefficients + m_params.num_expression_coefficients;

	cuComputeJacobianSparseFeatures << <1, threads >> > (
		//shared memory
		nFeatures,
		nShapeCoeffs, nExpressionCoeffs,
		nUnknowns, nResiduals,
		nVerticesTimes3, nShapeCoeffsTotal, nExpressionCoeffsTotal,
		regularizationWeight,

		face_pose, drx, dry, drz, projection, jacobian_local,

		//device memory input
		prior_local_ids, current_face, sparse_features,
		p_shape_basis, p_expression_basis, p_coefficients_shape, p_coefficients_expression,

		//device memory output
		p_jacobian, p_residuals
		);

	cudaDeviceSynchronize();
}

__global__ void cuComputeJacobiPreconditioner(const int nUnknowns, const int nResiduals, float* p_jacobian, float* p_preconditioner)
{
	extern __shared__ float temp[];

	int col = blockIdx.x;
	int row = threadIdx.x;
	float v = p_jacobian[col * nResiduals + row];
	temp[row] = v * v;

	__syncthreads();

	if (threadIdx.x == 0)
	{
		float sum = 0;
		for (int i = 0; i < nResiduals; i++)
		{
			sum += temp[i];
		}
		p_preconditioner[col] = 1.0f / (fmaxf(2.0f*sum, 1e-8f));
	}

}

__global__ void cuElementwiseMultiplication(float* v1, float* v2, float* out)
{
	int i = util::getThreadIndex1D();
	out[i] = v1[i] * v2[i];
}

__global__ void textureRgbTestKernel(cudaTextureObject_t texture, float* arr, int width, int height)
{
	auto index = util::getThreadIndex2D();

	if (index.x >= width || index.y >= height)
	{
		return;
	}

	int y = height - 1 - index.y; // "height - 1 - index.y" is used since OpenGL uses left-bottom corner as texture origin.
	float4 color = tex2D<float4>(texture, index.x, y);

	auto idx = (index.x + index.y * width) * 3;
	arr[idx] = color.x;
	arr[idx + 1] = color.y;
	arr[idx + 2] = color.z;
}

__global__ void textureBarycentricsVertexIdsTestKernel(cudaTextureObject_t texture_barycentrics, cudaTextureObject_t texture_vertex_ids, glm::vec3* albedos,
	float* arr, int width, int height)
{
	auto index = util::getThreadIndex2D();

	if (index.x >= width || index.y >= height)
	{
		return;
	}

	int y = height - 1 - index.y; // "height - 1 - index.y" is used since OpenGL uses left-bottom corner as texture origin.
	float4 barycentrics_light = tex2D<float4>(texture_barycentrics, index.x, y); // barycentrics_light.w is light.
	int4 vertex_ids = tex2D<int4>(texture_vertex_ids, index.x, y);

	auto albedo_v0 = albedos[vertex_ids.x];
	auto albedo_v1 = albedos[vertex_ids.y];
	auto albedo_v2 = albedos[vertex_ids.z];

	auto albedo = barycentrics_light.x * albedo_v0 + barycentrics_light.y * albedo_v1 + barycentrics_light.z * albedo_v2;
	auto color = albedo * barycentrics_light.w;

	auto idx = (index.x + index.y * width) * 3;
	arr[idx] = color.x;
	arr[idx + 1] = color.y;
	arr[idx + 2] = color.z;
}

void GaussNewtonSolver::computeJacobiPreconditioner(const int nUnknowns, const int nResiduals, float* p_jacobian, float* p_preconditioner)
{
	//TODO: split this up into proper blocks, once we have more that 1024 resiudals 
	cuComputeJacobiPreconditioner << <nUnknowns, nResiduals, sizeof(float)*nResiduals >> > (nUnknowns, nResiduals, p_jacobian, p_preconditioner);
	cudaDeviceSynchronize();
}

void GaussNewtonSolver::elementwiseMultiplication(const int nElements, float* v1, float* v2, float* out)
{
	cuElementwiseMultiplication << <1, nElements >> > (v1, v2, out);
	cudaDeviceSynchronize();
}

void GaussNewtonSolver::debugFrameBufferTextures(Face& face, const std::string& rgb_filepath, const std::string& deferred_filepath)
{
	int img_width = face.m_graphics_settings.screen_width;
	int img_height = face.m_graphics_settings.screen_height;
	util::DeviceArray<float> temp_memory(img_width * img_height * 3);

	dim3 threads(16, 16);
	dim3 blocks(img_width / threads.x + 1, img_height / threads.y + 1);

	textureRgbTestKernel << <blocks, threads >> > (m_texture_rgb, temp_memory.getPtr(), img_width, img_height);

	std::vector<float> temp_memory_host(img_width * img_height * 3);
	util::copy(temp_memory_host, temp_memory, temp_memory.getSize());

	cv::Mat image(cv::Size(img_width, img_height), CV_8UC3);
	for (int y = 0; y < image.rows; y++)
	{
		for (int x = 0; x < image.cols; x++)
		{
			auto idx = (x + y * img_width) * 3;

			// OpenCV expects it to be an BGRA image.
			image.at<cv::Vec3b>(cv::Point(x, y)) = cv::Vec3b(255.0f * cv::Vec3f(temp_memory_host[idx + 2], temp_memory_host[idx + 1], temp_memory_host[idx]));
		}
	}
	cv::imwrite(rgb_filepath, image);

	textureBarycentricsVertexIdsTestKernel << <blocks, threads >> > (m_texture_barycentrics, m_texture_vertex_ids, face.m_current_face_gpu.getPtr() + face.m_number_of_vertices,
		temp_memory.getPtr(), img_width, img_height);

	util::copy(temp_memory_host, temp_memory, temp_memory.getSize());
	for (int y = 0; y < image.rows; y++)
	{
		for (int x = 0; x < image.cols; x++)
		{
			auto idx = (x + y * img_width) * 3;

			// OpenCV expects it to be an BGRA image.
			image.at<cv::Vec3b>(cv::Point(x, y)) = cv::Vec3b(255.0f * cv::Vec3f(temp_memory_host[idx + 2], temp_memory_host[idx + 1], temp_memory_host[idx]));
		}
	}
	cv::imwrite(deferred_filepath, image);
}