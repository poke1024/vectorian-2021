#ifndef __VECTORIAN_CONTEXTUAL_EMBEDDING_METRIC_H__
#define __VECTORIAN_CONTEXTUAL_EMBEDDING_METRIC_H__

#include "metric/metric.h"
#include "embedding/contextual.h"

class ContextualEmbeddingMetric : public Metric {
public:
	inline ContextualEmbeddingMetric(
		const std::string &p_name,
		const SimilarityMatrixRef &p_matrix,
		const MatcherFactoryRef &p_matcher_factory) :

		Metric(p_name, p_matrix, p_matcher_factory) {
	}

	virtual MetricRef clone(const SimilarityMatrixRef &p_matrix) {
		return std::make_shared<ContextualEmbeddingMetric>(
			m_name, p_matrix, m_matcher_factory);
	}
};

typedef std::shared_ptr<ContextualEmbeddingMetric> ContextualEmbeddingMetricRef;

class ContextualEmbeddingMetricFactory {

	py::dict m_sent_metric_def;
	ContextualEmbeddingRef m_embedding;

	MatcherFactoryRef create_matcher_factory(
		const QueryRef &p_query,
		const WordMetricDef &p_metric);

	inline const py::dict &sent_metric_def() const {
		return m_sent_metric_def;
	}

	inline py::dict alignment_def() const {
		return m_sent_metric_def["alignment"].cast<py::dict>();
	}

public:
	ContextualEmbeddingMetricFactory(
		const ContextualEmbeddingRef &p_embedding,
		const py::dict &p_sent_metric_def) :

		m_sent_metric_def(p_sent_metric_def),
		m_embedding(p_embedding) {
	}

	ContextualEmbeddingMetricRef create(
		const QueryRef &p_query,
		const WordMetricDef &p_metric);
};


class ContextualEmbeddingSimilarityMatrixFactory : public SimilarityMatrixFactory {
	const QueryRef m_query;
	const WordMetricDef m_metric;
	const MatcherFactoryRef m_matcher_factory;
	const size_t m_embedding_index;

	SimilarityMatrixRef build_similarity_matrix(
		const std::vector<StaticEmbeddingRef> &p_embeddings);

	void compute_magnitudes(
		const std::vector<StaticEmbeddingRef> &p_embeddings,
		const SimilarityMatrixRef &p_matrix);

public:
	ContextualEmbeddingSimilarityMatrixFactory(
		const QueryRef &p_query,
		const WordMetricDef &p_metric,
		const MatcherFactoryRef &p_matcher_factory,
		const size_t p_embedding_index) :

		m_query(p_query),
		m_metric(p_metric),
		m_matcher_factory(p_matcher_factory),
		m_embedding_index(p_embedding_index) {
	}

	virtual SimilarityMatrixRef create(
		const DocumentRef &p_document);
};

typedef std::shared_ptr<StaticEmbeddingSimilarityMatrixFactory> StaticEmbeddingSimilarityMatrixFactoryRef;

#endif // __VECTORIAN_CONTEXTUAL_EMBEDDING_METRIC_H__
