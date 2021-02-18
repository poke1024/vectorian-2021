#include "embedding/sim.h"

struct Cosine {
	inline float operator()(
		const WordVectors &p_vectors,
		token_t p_s,
		token_t p_t) const {

		return p_vectors.normalized.row(p_s).dot(p_vectors.normalized.row(p_t));
	}
};

struct ZhuCosine { // Zhu et al.
	inline float operator()(
		const WordVectors &p_vectors,
		token_t p_s,
		token_t p_t) const {

		const float num = (p_vectors.normalized.row(p_s) * p_vectors.normalized.row(p_t)).array().sqrt().sum();
		const float denom = p_vectors.normalized.row(p_s).sum() * p_vectors.normalized.row(p_t).sum();
		return num / denom;
	}
};

struct SohangirCosine { // Sohangir & Wang
	inline float operator()(
		const WordVectors &p_vectors,
		token_t p_s,
		token_t p_t) const {

		const float num = (p_vectors.raw.row(p_s) * p_vectors.raw.row(p_t)).array().sqrt().sum();
		const float denom = std::sqrt(p_vectors.normalized.row(p_s).sum()) * std::sqrt(p_vectors.normalized.row(p_t).sum());
		return num / denom;
	}
};

struct PNorm {
	const float m_p;
	const float m_distance_scale;

	inline PNorm(float p = 2.0f, float scale = 1.0f) : m_p(p), m_distance_scale(scale) {
	}

	inline float operator()(
		const WordVectors &p_vectors,
		token_t p_s,
		token_t p_t) const {

		const auto uv = p_vectors.raw.row(p_s) - p_vectors.raw.row(p_t);
		const float distance = pow(uv.cwiseAbs().array().pow(m_p).sum(), 1.0f / m_p);
		return std::max(0.0f, 1.0f - distance * m_distance_scale);
	}
};

EmbeddingSimilarityRef MetricDef::instantiate(
	const WordVectors &p_vectors) const {

	if (metric == "cosine") {
		return std::make_shared<SimilarityMeasure<Cosine>>(p_vectors);
	} if (metric == "zhu-cosine") {
		return std::make_shared<SimilarityMeasure<ZhuCosine>>(p_vectors);
	} if (metric == "sohangir-cosine") {
		return std::make_shared<SimilarityMeasure<SohangirCosine>>(p_vectors);
	} else if (metric == "p-norm") {
		return std::make_shared<SimilarityMeasure<PNorm>>(
			p_vectors, PNorm(
				options["p"].cast<float>(),
				options["scale"].cast<float>()));
	} else {
		std::ostringstream err;
		err << "unsupported metric " << metric;
		throw std::runtime_error(err.str());
	}
}