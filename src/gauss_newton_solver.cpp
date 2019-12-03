#include "gauss_newton_solver.h"
#include "prior_sparse_features.h"
#include "util.h"
#include "device_util.h"
#include "device_array.h"
#include <Eigen/Dense>
#include <chrono>


GaussNewtonSolver::GaussNewtonSolver()
{
	cublasCreate(&m_cublas); 
}

GaussNewtonSolver::~GaussNewtonSolver()
{
	cublasDestroy(m_cublas); 
}

void GaussNewtonSolver::solve_CPU(const std::vector<glm::vec2>& sparse_features, Face& face, glm::mat4& projection)
{

	if (sparse_features.empty()) //no tracking -> cublas doesnt like a getting matrix/vector of size 0
		return; 

	int nFeatures = sparse_features.size();
	int nResiduals = 2 * nFeatures;
	int nShapeCoeffs = m_params.num_shape_coefficients;
	int nExpressionCoeffs = m_params.num_expression_coefficients; 
	int nFaceCoeffs = nShapeCoeffs + nExpressionCoeffs; 
	int nUnknowns = 7 + nFaceCoeffs; //3+3+1 = 7 DoF for rotation, translation and intrinsics.
	nResiduals += nFaceCoeffs; //Regularizer

	float wReg = std::powf(10, m_params.regularisation_weight_exponent); 


	//const auto& prior_local_positions = PriorSparseFeatures::get().getPriorPositions();
	const auto& prior_local_ids = PriorSparseFeatures::get().getPriorIds();
	auto& rotation_coefficients = face.getRotationCoefficients();
	auto& translation_coefficients = face.getTranslationCoefficients();

	Eigen::VectorXf residuals(nResiduals);
	Eigen::MatrixXf jacobian(nResiduals, nUnknowns); 
	jacobian.setZero(); 

	auto jacobian_gpu = util::DeviceArray<float>(nUnknowns*nResiduals);
	auto residuals_gpu = util::DeviceArray<float>(nResiduals);
	auto result_gpu = util::DeviceArray<float>(nUnknowns);
	std::vector<float> result(nUnknowns);

	
	//auto ids_gpu = util::DeviceArray<int>(prior_local_ids);
	//auto keyPts_gpu = util::DeviceArray<glm::vec2>(sparse_features);
	//auto result_coeffs_gpu = util::DeviceArray<float>(nFaceCoeffs);

	Eigen::Map<Eigen::MatrixXf> shape_basis(face.m_shape_basis.data(), face.m_number_of_vertices * 3, face.m_shape_coefficients.size());
	Eigen::Map<Eigen::MatrixXf> expression_basis(face.m_expression_basis.data(), face.m_number_of_vertices * 3, face.m_expression_coefficients.size());

	//auto& shape_basis = face.m_shape_coefficients; 
	//auto& expression_basis = face.m_expression_basis; 

	//Some parts of jacobians are constants. That's why thet are intialized here only once.
	//Do not touch them inside the for loops.
	Eigen::Matrix<float, 2, 3> jacobian_proj;
	jacobian_proj(0, 1) = 0.0f;
	jacobian_proj(1, 0) = 0.0f;

	Eigen::Matrix<float, 3, 3> jacobian_world;
	jacobian_world(0, 1) = 0.0f;
	jacobian_world(0, 2) = 0.0f;
	jacobian_world(1, 0) = 0.0f;
	jacobian_world(1, 1) = projection[1][1];
	jacobian_world(1, 2) = 0.0f;
	jacobian_world(2, 0) = 0.0f;
	jacobian_world(2, 1) = 0.0f;
	jacobian_world(2, 2) = -1.0f;

	Eigen::Matrix<float, 3, 1> jacobian_intrinsics;
	jacobian_intrinsics(1, 0) = 0.0f;
	jacobian_intrinsics(2, 0) = 0.0f;

	Eigen::Matrix<float, 3, 6> jacobian_pose;
	jacobian_pose(0, 3) = 1.0f;
	jacobian_pose(1, 3) = 0.0f;
	jacobian_pose(2, 3) = 0.0f;
	jacobian_pose(0, 4) = 0.0f;
	jacobian_pose(1, 4) = 1.0f;
	jacobian_pose(2, 4) = 0.0f;
	jacobian_pose(0, 5) = 0.0f;
	jacobian_pose(1, 5) = 0.0f;
	jacobian_pose(2, 5) = 1.0f;

	Eigen::Matrix<float, 3, 3> jacobian_local;

	//clear since we are tracking to model right now, so gradients wrt. eigenvalues are given wrt. average face
	//for (int i = 0; i < nShapeCoeffs; ++i)
	//{
	//	face.m_shape_coefficients[i] = 0;
	//}
	//for (int i = 0; i < nExpressionCoeffs; ++i)
	//{
	//	face.m_expression_coefficients[i] = 0;
	//}



	for (int iteration = 0; iteration < m_params.num_gn_iterations; ++iteration)
	{
		face.computeFace();
		std::vector<glm::vec3> current_face(face.m_number_of_vertices);
		util::copy(current_face, face.m_current_face_gpu, face.m_number_of_vertices);

		auto face_pose = face.computeModelMatrix();
		jacobian_local <<
			face_pose[0][0], face_pose[1][0], face_pose[2][0],
			face_pose[0][1], face_pose[1][1], face_pose[2][1],
			face_pose[0][2], face_pose[1][2], face_pose[2][2];

		glm::mat3 drx, dry, drz;
		face.computeRotationDerivatives(drx, dry, drz);

		//Construct residuals and jacobian for sparse features
		for (int i = 0; i < nFeatures; ++i)
		{
			auto vertexId = prior_local_ids[i]; 
			//auto local_coord = prior_local_positions[i];
			auto local_coord = current_face[vertexId];

			auto world_coord = face_pose * glm::vec4(local_coord, 1.0f);
			auto proj_coord = projection * world_coord;
			auto uv = glm::vec2(proj_coord.x, proj_coord.y) / proj_coord.w;

			//Residual
			auto residual = sparse_features[i] - uv;

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

			auto jacobian_shape =  jacobian_proj_world_local * shape_basis.block(3 * vertexId, 0, 3, nShapeCoeffs);

			jacobian.block(i * 2, 7, 2, nShapeCoeffs) = jacobian_shape; 

			auto jacobian_expression = jacobian_proj_world_local * expression_basis.block(3 * vertexId, 0, 3, nExpressionCoeffs);
			jacobian.block(i * 2, 7 + nShapeCoeffs, 2, nExpressionCoeffs) = jacobian_expression;
		} //END sparse features jacobian + residuals

		//regularizer
		{
			int offset_cols_shape = 7; 
			int offset_rows_shape = 2*nFeatures;

			int offset_cols_expression = offset_cols_shape + nShapeCoeffs; 
			int offset_rows_expression = offset_rows_shape + nShapeCoeffs; 

			for (int i = 0; i < nShapeCoeffs; ++i)
			{
				float divSigma = 1.0f/face.m_shape_std_dev[i]; 
				jacobian(offset_rows_shape + i, offset_cols_shape + i) = divSigma * divSigma*face.m_shape_coefficients[i] * wReg * 2;
				residuals(offset_rows_shape + i) = 0; 
			}
			for (int i = 0; i < nExpressionCoeffs; ++i)
			{
				float divSigma = 1.0f / face.m_expression_std_dev[i];
				jacobian(offset_rows_expression + i, offset_cols_expression + i) = divSigma * divSigma*face.m_expression_coefficients[i] * wReg * 2;
				residuals(offset_rows_expression + i) = 0;
			}
		}

		//Apply step and update poses CPU
		/**/
		//auto jacobian_t = jacobian.transpose();
		//auto jtj = jacobian_t * jacobian;
		//auto jtr = -jacobian_t * residuals;

		//Eigen::JacobiSVD<Eigen::MatrixXf> svd(jtj, Eigen::ComputeThinU | Eigen::ComputeThinV);
		//auto result_eigen = svd.solve(jtr);
		// projection[0][0] -= result_eigen(0);

		//rotation_coefficients.x -= result_eigen(1);
		//rotation_coefficients.y -= result_eigen(2);
		//rotation_coefficients.z -= result_eigen(3);

		//translation_coefficients.x -= result_eigen(4);
		//translation_coefficients.y -= result_eigen(5);
		//translation_coefficients.z -= result_eigen(6);
		/**/

		//Apply step and update poses GPU

		util::copy(jacobian_gpu, jacobian.data(), nUnknowns*nResiduals); 
		util::copy(residuals_gpu, residuals.data(), nResiduals);

		solveUpdatePCG(m_cublas, nUnknowns, nResiduals, jacobian_gpu, residuals_gpu, result_gpu, 2, -1);
		util::copy(result, result_gpu, nUnknowns);


		projection[0][0] -= result[0];

		rotation_coefficients.x -= result[1];
		rotation_coefficients.y -= result[2];
		rotation_coefficients.z -= result[3];

		translation_coefficients.x -= result[4];
		translation_coefficients.y -= result[5];
		translation_coefficients.z -= result[6];


		float sca = 1;
#pragma omp parallel for
		for (int i = 0; i < nShapeCoeffs; ++i)
		{
			auto c = face.m_shape_coefficients[i] - result[7 + i] * sca / face.m_shape_std_dev[i];
			face.m_shape_coefficients[i] = c;
		}
#pragma omp parallel for
		for (int i = 0; i < nExpressionCoeffs; ++i)
		{
			auto c = face.m_expression_coefficients[i] - result[7 + nShapeCoeffs + i] * sca / face.m_expression_std_dev[i];
			face.m_expression_coefficients[i] = std::max(0.0f, std::min(1.f, c));
		}


		//if (iteration % 5 == 0)
		//{
			//std::cout << "Aspect Ratio: " << projection[1][1] / projection[0][0] << std::endl;
			//std::cout << "Unknowns: " << nUnknowns << ", Residuals: " << nResiduals << std::endl;
			//std::cout << "System Rank: " << svd.rank() << std::endl;
			////std::cout << "Result: " << result << std::endl;
			//std::cout << "Iteration: " << iteration << " , Loss: " << (residuals.array() * residuals.array()).sum() << std::endl;
		//}
			
	} //end GN step
}


void GaussNewtonSolver::solve(const std::vector<glm::vec2>& sparse_features, Face& face, glm::mat4& projection)
{

	if (sparse_features.empty()) //no tracking -> cublas doesnt like a getting matrix/vector of size 0
		return;

	const int nFeatures = sparse_features.size();
	const int nShapeCoeffs = m_params.num_shape_coefficients;
	const int nExpressionCoeffs = m_params.num_expression_coefficients;
	const int nFaceCoeffs = nShapeCoeffs + nExpressionCoeffs;
	const int nUnknowns = 7 + nFaceCoeffs; //3+3+1 = 7 DoF for rotation, translation and intrinsics.
	const int nResiduals = 2 * nFeatures + nFaceCoeffs;


	float regulatization_weigth = std::powf(10, m_params.regularisation_weight_exponent);

	const auto& prior_local_ids = PriorSparseFeatures::get().getPriorIds();
	auto& rotation_coefficients = face.getRotationCoefficients();
	auto& translation_coefficients = face.getTranslationCoefficients();


	auto jacobian_gpu = util::DeviceArray<float>(nUnknowns*nResiduals);
	auto residuals_gpu = util::DeviceArray<float>(nResiduals);
	auto result_gpu = util::DeviceArray<float>(nUnknowns);
	std::vector<float> result(nUnknowns);


	auto ids_gpu = util::DeviceArray<int>(prior_local_ids);
	auto keyPts_gpu = util::DeviceArray<glm::vec2>(sparse_features);

	//Some parts of jacobians are constants. That's why thet are intialized here only once.
	//Do not touch them inside the for loops.
	Eigen::Matrix<float, 2, 3> jacobian_proj;
	jacobian_proj(0, 1) = 0.0f;
	jacobian_proj(1, 0) = 0.0f;

	Eigen::Matrix<float, 3, 3> jacobian_world;
	jacobian_world(0, 1) = 0.0f;
	jacobian_world(0, 2) = 0.0f;
	jacobian_world(1, 0) = 0.0f;
	jacobian_world(1, 1) = projection[1][1];
	jacobian_world(1, 2) = 0.0f;
	jacobian_world(2, 0) = 0.0f;
	jacobian_world(2, 1) = 0.0f;
	jacobian_world(2, 2) = -1.0f;

	Eigen::Matrix<float, 3, 1> jacobian_intrinsics;
	jacobian_intrinsics(1, 0) = 0.0f;
	jacobian_intrinsics(2, 0) = 0.0f;

	Eigen::Matrix<float, 3, 6> jacobian_pose;
	jacobian_pose(0, 3) = 1.0f;
	jacobian_pose(1, 3) = 0.0f;
	jacobian_pose(2, 3) = 0.0f;
	jacobian_pose(0, 4) = 0.0f;
	jacobian_pose(1, 4) = 1.0f;
	jacobian_pose(2, 4) = 0.0f;
	jacobian_pose(0, 5) = 0.0f;
	jacobian_pose(1, 5) = 0.0f;
	jacobian_pose(2, 5) = 1.0f;

	Eigen::Matrix<float, 3, 3> jacobian_local;

	//clear if we are tracking to model, so gradients wrt. eigenvalues are given wrt. average face
	//for (int i = 0; i < nShapeCoeffs; ++i)
	//{
	//	face.m_shape_coefficients[i] = 0;
	//}
	//for (int i = 0; i < nExpressionCoeffs; ++i)
	//{
	//	face.m_expression_coefficients[i] = 0;
	//}


	for (int iteration = 0; iteration < m_params.num_gn_iterations; ++iteration)
	{

		face.computeFace();


		auto face_pose = face.computeModelMatrix();
		jacobian_local <<
			face_pose[0][0], face_pose[1][0], face_pose[2][0],
			face_pose[0][1], face_pose[1][1], face_pose[2][1],
			face_pose[0][2], face_pose[1][2], face_pose[2][2];

		glm::mat3 drx, dry, drz;
		face.computeRotationDerivatives(drx, dry, drz);

		//CUDA
		//TODO: block stuff
		computeJacobianSparseFeatures(
			//shared memory
			nFeatures, nShapeCoeffs, nExpressionCoeffs, nUnknowns, nResiduals,
			face.m_number_of_vertices * 3, face.m_shape_coefficients.size(), face.m_expression_coefficients.size(),
			face_pose, drx, dry, drz, projection,
			jacobian_proj, jacobian_world, jacobian_intrinsics, jacobian_pose, jacobian_local,

			//device memory input
			ids_gpu.getPtr(), face.m_current_face_gpu.getPtr(), keyPts_gpu.getPtr(),
			face.m_shape_basis_gpu.getPtr(), face.m_expression_basis_gpu.getPtr(),

			//device memory output
			jacobian_gpu.getPtr(), residuals_gpu.getPtr()
			); 

		computeRegularizer(face, 2 * nFeatures, nUnknowns, nResiduals, regulatization_weigth, jacobian_gpu.getPtr(), residuals_gpu.getPtr()); 

		//Apply step and update poses GPU
		//solveUpdateCG(m_cublas, nUnknowns, nResiduals, jacobian_gpu, residuals_gpu, result_gpu, 2, -1);
		solveUpdatePCG(m_cublas, nUnknowns, nResiduals, jacobian_gpu, residuals_gpu, result_gpu, 2, -1);
		//solveUpdateLU(m_cublas, nUnknowns, nResiduals, jacobian_gpu, residuals_gpu, result_gpu, 2, -1);
		util::copy(result, result_gpu, nUnknowns);


		projection[0][0] -= result[0];

		rotation_coefficients.x -= result[1];
		rotation_coefficients.y -= result[2];
		rotation_coefficients.z -= result[3];

		translation_coefficients.x -= result[4];
		translation_coefficients.y -= result[5];
		translation_coefficients.z -= result[6];

		float sca = 1;
#pragma omp parallel for
		for (int i = 0; i < nShapeCoeffs; ++i)
		{
			auto c = face.m_shape_coefficients[i] - result[7 + i] * sca / face.m_shape_std_dev[i];
			face.m_shape_coefficients[i] = c;
		}
#pragma omp parallel for
		for (int i = 0; i < nExpressionCoeffs; ++i)
		{
			auto c = face.m_expression_coefficients[i] - result[7 + nShapeCoeffs + i] * sca / face.m_expression_std_dev[i];
			face.m_expression_coefficients[i] = std::max(0.0f, std::min(1.f, c));
		}
	}
}

void GaussNewtonSolver::solveUpdateLU(const cublasHandle_t& cublas, const int nUnknowns, const int nResiduals, util::DeviceArray<float>& jacobian, util::DeviceArray<float>& residuals, util::DeviceArray<float>& result, const float alphaLHS, const float alphaRHS)
{

	float alpha = 1, beta = 0;

	////transpose jacobian bc of stupid col major cublas BS
	//auto jacobian = util::DeviceArray<float>(nUnknowns*nResiduals);
	//cublasSgeam(cublas, CUBLAS_OP_T, CUBLAS_OP_N, nResiduals, nUnknowns, &alpha, jacobianT.getPtr(), nUnknowns, &beta, jacobian.getPtr(), nResiduals, jacobian.getPtr(), nResiduals);



	//solve JTJd = JTf by computeing JTJ and JTf and using cublas LU solver(very bad)
	auto& JTf = result;
	auto JTJ = util::DeviceArray<float>(nUnknowns*nUnknowns);
	auto JTJinv = util::DeviceArray<float>(nUnknowns*nUnknowns);
	JTJ.memset(0);

	alpha = alphaRHS, beta = 0;
	//JTf
	cublasSgemv(cublas, CUBLAS_OP_T, nResiduals, nUnknowns, &alpha, jacobian.getPtr(), nResiduals, residuals.getPtr(), 1, &beta, JTf.getPtr(), 1);
	alpha = alphaLHS, beta = 0;
	//JTJ
	cublasSgemm(cublas, CUBLAS_OP_T, CUBLAS_OP_N, nUnknowns, nUnknowns, nResiduals, &alpha, jacobian.getPtr(), nResiduals, jacobian.getPtr(), nResiduals, &beta, JTJ.getPtr(), nUnknowns);

	cublasSetPointerMode(cublas, CUBLAS_POINTER_MODE_DEVICE);

	//int info = 0;
	auto batch = util::DeviceArray<float*>({ JTJ.getPtr() });
	auto info = util::DeviceArray<int>(1);
	auto pivot = util::DeviceArray<int>(nUnknowns);

	cublasSgetrfBatched(cublas, nUnknowns, batch.getPtr(), nUnknowns, pivot.getPtr(), info.getPtr(), 1);

	auto ibatch = util::DeviceArray<float*>({ JTJinv.getPtr() });
	cublasSgetriBatched(cublas, nUnknowns, batch.getPtr(), nUnknowns, pivot.getPtr(), ibatch.getPtr(), nUnknowns, info.getPtr(), 1);

	cublasSetPointerMode(cublas, CUBLAS_POINTER_MODE_HOST);
	alpha = 1, beta = 0;
	cublasSgemv(cublas, CUBLAS_OP_N, nUnknowns, nUnknowns, &alpha, JTJinv.getPtr(), nUnknowns, JTf.getPtr(), 1, &beta, result.getPtr(), 1);


	/*cublasStrsm(cublas, CUBLAS_SIDE_LEFT, CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_N, CUBLAS_DIAG_UNIT, nUnknowns, 1, &alpha, JTJ.getPtr(), nUnknowns, JTf.getPtr(), nUnknowns);
	cublasStrsm(cublas, CUBLAS_SIDE_LEFT, CUBLAS_FILL_MODE_UPPER, CUBLAS_OP_N, CUBLAS_DIAG_NON_UNIT, nUnknowns, 1, &alpha, JTJ.getPtr(), nUnknowns, JTf.getPtr(), nUnknowns);
	*/


}

void GaussNewtonSolver::solveUpdatePCG(const cublasHandle_t& cublas, const int nUnknowns, const int nResiduals, util::DeviceArray<float>& jacobian, util::DeviceArray<float>& residuals, util::DeviceArray<float>& x, const float alphaLHS, const float alphaRHS)
{
	const float alpha = 1, beta = 0;

	x.memset(0);
	auto r = util::DeviceArray<float>(nUnknowns);	//current residual
	auto p = util::DeviceArray<float>(nUnknowns);	//gradient 
	auto M = util::DeviceArray<float>(nUnknowns);	//preconditioner
	auto z = util::DeviceArray<float>(nUnknowns);	//preconditioned residual

	auto Jp = util::DeviceArray<float>(nResiduals);
	auto JTJp = util::DeviceArray<float>(nUnknowns);

	//M=inv(2*diag(JTJ))
	computeJacobiPreconditioner(nUnknowns, nResiduals, jacobian.getPtr(), M.getPtr()); 

	//r = JTf;
	cublasSgemv(cublas, CUBLAS_OP_T, nResiduals, nUnknowns, &alphaRHS, jacobian.getPtr(), nResiduals, residuals.getPtr(), 1, &beta, r.getPtr(), 1);

	//z = Mr
	elementwiseMultiplication(nUnknowns, M.getPtr(), r.getPtr(), z.getPtr());

	//p=z;
	cublasScopy(cublas, nUnknowns, z.getPtr(), 1, p.getPtr(), 1);

	float zTr_old = 0, zTr = 0;
	float pTJTJp;
	//zTr
	cublasSdot(cublas, nUnknowns, z.getPtr(), 1, r.getPtr(), 1, &zTr_old);
	int i = 0;
	for (; i < std::min(nUnknowns, m_params.num_pcg_iterations); ++i)
	{
		//apply JTJ
		cublasSgemv(cublas, CUBLAS_OP_N, nResiduals, nUnknowns, &alphaLHS, jacobian.getPtr(), nResiduals, p.getPtr(), 1, &beta, Jp.getPtr(), 1);
		cublasSgemv(cublas, CUBLAS_OP_T, nResiduals, nUnknowns, &alpha, jacobian.getPtr(), nResiduals, Jp.getPtr(), 1, &beta, JTJp.getPtr(), 1);

		cublasSdot(cublas, nUnknowns, p.getPtr(), 1, JTJp.getPtr(), 1, &pTJTJp);

		float ak = zTr_old / std::max(pTJTJp, m_params.kNearZero);
		//x = ak*p + x
		cublasSaxpy(cublas, nUnknowns, &ak, p.getPtr(), 1, x.getPtr(), 1);

		//r = r - ak* JTJp
		ak *= -1;
		cublasSaxpy(cublas, nUnknowns, &ak, JTJp.getPtr(), 1, r.getPtr(), 1);

		//z=Mr
		elementwiseMultiplication(nUnknowns, M.getPtr(), r.getPtr(), z.getPtr());

		//zTr
		cublasSdot(cublas, nUnknowns, z.getPtr(), 1, r.getPtr(), 1, &zTr);

		if (zTr < m_params.kTolerance) break;

		float bk = zTr / std::max(zTr_old, m_params.kNearZero);

		//p = z + bk*p        
		cublasSscal(cublas, nUnknowns, &bk, p.getPtr(), 1);
		cublasSaxpy(cublas, nUnknowns, &alpha, z.getPtr(), 1, p.getPtr(), 1);

		zTr_old = zTr;
	}
//	std::cout << "PCG iters: " << i << std::endl; 
}

void GaussNewtonSolver::solveUpdateCG(const cublasHandle_t& cublas, const int nUnknowns, const int nResiduals, util::DeviceArray<float>& jacobian, util::DeviceArray<float>& residuals, util::DeviceArray<float>& x, const float alphaLHS, const float alphaRHS)
{
	const float alpha = 1, beta = 0;

	x.memset(0);
	//r = JTf;
	auto r = util::DeviceArray<float>(nUnknowns);	//current residual
	auto p = util::DeviceArray<float>(nUnknowns);	//gradient 
	auto Jp = util::DeviceArray<float>(nResiduals);
	auto JTJp = util::DeviceArray<float>(nUnknowns);
	cublasSgemv(cublas, CUBLAS_OP_T, nResiduals, nUnknowns, &alphaRHS, jacobian.getPtr(), nResiduals, residuals.getPtr(), 1, &beta, r.getPtr(), 1);
	//p=r;
	cublasScopy(cublas, nUnknowns, r.getPtr(), 1, p.getPtr(), 1);

	float rTr_old = 0, rTr;
	float pTJTJp;
	//rTr
	cublasSdot(cublas, nUnknowns, r.getPtr(), 1, r.getPtr(), 1, &rTr);
	int i = 0;
	for (; i < std::min(nUnknowns, m_params.num_pcg_iterations); ++i)
	{
		//apply JTJ
		cublasSgemv(cublas, CUBLAS_OP_N, nResiduals, nUnknowns, &alphaLHS, jacobian.getPtr(), nResiduals, p.getPtr(), 1, &beta, Jp.getPtr(), 1);
		cublasSgemv(cublas, CUBLAS_OP_T, nResiduals, nUnknowns, &alpha, jacobian.getPtr(), nResiduals, Jp.getPtr(), 1, &beta, JTJp.getPtr(), 1);

		rTr_old = rTr;

		cublasSdot(cublas, nUnknowns, p.getPtr(), 1, JTJp.getPtr(), 1, &pTJTJp);

		float ak = rTr / std::max(pTJTJp, m_params.kNearZero);
		//x = ak*p + x
		cublasSaxpy(cublas, nUnknowns, &ak, p.getPtr(), 1, x.getPtr(), 1);

		//r = r - ak* JTJp
		ak *= -1;
		cublasSaxpy(cublas, nUnknowns, &ak, JTJp.getPtr(), 1, r.getPtr(), 1);

		//rTr
		cublasSdot(cublas, nUnknowns, r.getPtr(), 1, r.getPtr(), 1, &rTr);

		if (rTr < m_params.kTolerance) break;

		float bk = rTr / std::max(rTr_old, m_params.kNearZero);

		//p = r + bk*p        
		cublasSscal(cublas, nUnknowns, &bk, p.getPtr(), 1);
		cublasSaxpy(cublas, nUnknowns, &alpha, r.getPtr(), 1, p.getPtr(), 1);

	}
	//std::cout << "CG iters: " << i << std::endl;
}