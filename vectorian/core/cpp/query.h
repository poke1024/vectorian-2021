#ifndef __VECTORIAN_QUERY_H__
#define __VECTORIAN_QUERY_H__

#include "common.h"
#include "utils.h"
#include "metric/composite.h"
#include "vocabulary.h"

class Query : public std::enable_shared_from_this<Query> {
	std::vector<MetricRef> m_metrics;
	const std::string m_text;
	TokenVectorRef m_t_tokens;
	POSWMap m_pos_weights;
	std::vector<float> m_t_tokens_pos_weights;
	float m_total_score;
	std::string m_cost_combine_function;
	int m_mismatch_length_penalty;
	float m_submatch_weight;
	bool m_bidirectional;
	bool m_ignore_determiners;
	bool m_aborted;

	/*void init_boost(const py::kwargs &p_kwargs) {
		m_t_boost.reserve(m_t_tokens.size());

		if (p_kwargs && p_kwargs.contains("t_boost")) {
			auto t_boost = p_kwargs["t_boost"].cast<py::list>();
			for (size_t i = 0; i < m_t_tokens.size(); i++) {
				m_t_boost.push_back(t_boost[i].cast<float>());
			}
		} else {
			for (size_t i = 0; i < m_t_tokens.size(); i++) {
				m_t_boost.push_back(1.0f);
			}
		}
	}*/

public:
	Query(
		VocabularyRef p_vocab,
		const std::string &p_text,
		py::handle p_tokens_table,
		py::kwargs p_kwargs) : m_text(p_text), m_aborted(false) {

		const std::shared_ptr<arrow::Table> table(
		    unwrap_table(p_tokens_table.ptr()));

		m_t_tokens = unpack_tokens(
			p_vocab, DO_NOT_MODIFY_VOCABULARY, p_text, table);

		//init_boost(p_kwargs);

		static const std::set<std::string> valid_options = {
			"pos_mismatch_penalty",
			"pos_weights",
			"similarity_falloff",
			"similarity_threshold",
			"metrics",
			"similarity_measure",
			"submatch_weight",
			"bidirectional",
			"ignore_determiners",
			"idf_weight",
			"mismatch_length_penalty",
			"cost_combine_function"
		};

		if (p_kwargs) {
			for (auto item : p_kwargs) {
				const std::string name = py::str(item.first);
				if (valid_options.find(name) == valid_options.end()) {
					std::ostringstream err;
					err << "illegal option " << name;
					throw std::runtime_error(err.str());
				}
#if 0
				const std::string value = py::str(item.second);
				std::cout << "received param " << name << ": " <<
					value << "\n";
#endif
			}
		}

		const float pos_mismatch_penalty =
			(p_kwargs && p_kwargs.contains("pos_mismatch_penalty")) ?
				p_kwargs["pos_mismatch_penalty"].cast<float>() :
				1.0f;

		const float similarity_threshold = (p_kwargs && p_kwargs.contains("similarity_threshold")) ?
            p_kwargs["similarity_threshold"].cast<float>() :
            0.0f;

		const float similarity_falloff = (p_kwargs && p_kwargs.contains("similarity_falloff")) ?
            p_kwargs["similarity_falloff"].cast<float>() :
            1.0f;

		m_submatch_weight = (p_kwargs && p_kwargs.contains("submatch_weight")) ?
            p_kwargs["submatch_weight"].cast<float>() :
            0.0f;

		m_bidirectional = (p_kwargs && p_kwargs.contains("bidirectional")) ?
            p_kwargs["bidirectional"].cast<bool>() :
            false;

		m_ignore_determiners = (p_kwargs && p_kwargs.contains("ignore_determiners")) ?
            p_kwargs["ignore_determiners"].cast<bool>() :
            false;

		const float idf_weight = (p_kwargs && p_kwargs.contains("idf_weight")) ?
				p_kwargs["idf_weight"].cast<float>() :
				0.0f;

		std::set<std::string> needed_metrics;
		if (p_kwargs && p_kwargs.contains("metrics")) {
			auto given_metrics = p_kwargs["metrics"].cast<py::list>();
			for (const auto &item : given_metrics) {
				if (py::isinstance<py::str>(item)) {
					const std::string name = item.cast<py::str>();
					needed_metrics.insert(name);
				} else if (py::isinstance<py::tuple>(item)) {
					auto tuple = item.cast<py::tuple>();
					if (tuple.size() != 3) {
						throw std::runtime_error("expected 3-tuple as metric");
					}
					const std::string a = tuple[0].cast<py::str>();
					const std::string b = tuple[1].cast<py::str>();
					needed_metrics.insert(a);
					needed_metrics.insert(b);
				} else {
						throw std::runtime_error(
							"expected list of 3-tuples as metrics");
				}
			}
		}

		std::map<std::string, float> pos_weights;
		if (p_kwargs && p_kwargs.contains("pos_weights")) {
			auto pws = p_kwargs["pos_weights"].cast<py::dict>();
			for (const auto &pw : pws) {
				pos_weights[pw.first.cast<py::str>()] = pw.second.cast<py::float_>();
			}
		}

		m_pos_weights = p_vocab->mapped_pos_weights(pos_weights);

		m_t_tokens_pos_weights.reserve(m_t_tokens->size());
		for (size_t i = 0; i < m_t_tokens->size(); i++) {
			const Token &t = m_t_tokens->at(i);

			auto w = m_pos_weights.find(t.tag);
			float s;
			if (w != m_pos_weights.end()) {
				s = w->second;
			} else {
				s = 1.0f;
			}

			m_t_tokens_pos_weights.push_back(s);
		}

		m_total_score = 0.0f;
		for (float w : m_t_tokens_pos_weights) {
			m_total_score += w;
		}

		const std::string similarity_measure = (p_kwargs && p_kwargs.contains("similarity_measure")) ?
			p_kwargs["similarity_measure"].cast<py::str>() : "cosine";

		auto metrics = p_vocab->create_metrics(
			m_text,
			*m_t_tokens.get(),
			needed_metrics,
			similarity_measure,
			pos_mismatch_penalty,
			similarity_falloff,
			similarity_threshold,
			m_pos_weights,
			idf_weight);

		// metrics are specified as list (m1, m2, ...) were each m is
		// either the name of a metric, e.g. "fasttext", or a 3-tuple
		// that specifies a mix: ("fasttext", "wn2vec", 0.2)

		if (p_kwargs && p_kwargs.contains("metrics")) {
			auto given_metrics = p_kwargs["metrics"].cast<py::list>();
			for (const auto &item : given_metrics) {
				if (py::isinstance<py::str>(item)) {
					const std::string name = item.cast<py::str>();
					m_metrics.push_back(lookup_metric(metrics, name));
				} else if (py::isinstance<py::tuple>(item)) {
					auto tuple = item.cast<py::tuple>();
					if (tuple.size() != 3) {
						throw std::runtime_error("expected 3-tuple as metric");
					}
					const std::string a = tuple[0].cast<py::str>();
					const std::string b = tuple[1].cast<py::str>();
					const float t = tuple[2].cast<float>();
					m_metrics.push_back(std::make_shared<CompositeMetric>(
						lookup_metric(metrics, a),
						lookup_metric(metrics, b),
						t
					));
				} else {
						throw std::runtime_error(
							"expected list as specification for metrics");
				}
			}
		}

		if (p_kwargs && p_kwargs.contains("cost_combine_function")) {
			m_cost_combine_function = p_kwargs["cost_combine_function"].cast<std::string>();
		} else {
			m_cost_combine_function = "sum";
		}

		if (p_kwargs && p_kwargs.contains("mismatch_length_penalty")) {
			auto penalty =  p_kwargs["mismatch_length_penalty"];
			if (py::isinstance<py::str>(penalty)) {
				const std::string x = penalty.cast<py::str>();
				if (x == "off") {
					m_mismatch_length_penalty = -1.0f; // off
				} else {
					throw std::runtime_error(
						"illegal value for mismatch_length_penalty");
				}
			} else {
				m_mismatch_length_penalty = penalty.cast<float>();
			}
		} else {
			m_mismatch_length_penalty = 5;
		}
	}

	const std::string &text() const {
		return m_text;
	}

	inline const TokenVectorRef &tokens() const {
		return m_t_tokens;
	}

	inline int len() const {
		return m_t_tokens->size();
	}

	inline const POSWMap &pos_weights() const {
		return m_pos_weights;
	}

	const std::vector<MetricRef> &metrics() const {
		return m_metrics;
	}

	const std::string &cost_combine_function() const {
		return m_cost_combine_function;
	}

	inline int mismatch_length_penalty() const {
		return m_mismatch_length_penalty;
	}

	inline bool bidirectional() const {
		return m_bidirectional;
	}

	inline bool ignore_determiners() const {
	    return m_ignore_determiners;
	}

	ResultSetRef match(
		const DocumentRef &p_document);

	bool aborted() const {
		return m_aborted;
	}

	void abort() {
		m_aborted = true;
	}

	inline int max_matches() const {
		return 100;
	}

	inline float min_score() const {
		return 0.2f;
	}

	inline float reference_score(
		const float p_matched,
		const float p_unmatched) const {

		// m_matched_weight == 0 indicates that there
		// is no higher relevance of matched content than
		// unmatched content, both are weighted equal (see
		// maximum_internal_score()).

		const float unmatched_weight = std::pow(
			(m_total_score - p_matched) / m_total_score,
			m_submatch_weight);

		const float reference_score =
			p_matched +
			unmatched_weight * (m_total_score - p_matched);

		return reference_score;
	}

	template<typename CostCombine>
	inline float normalized_score(
		const float p_raw_score,
		const std::vector<int16_t> &p_match) const {

#if 0
		return p_raw_score / m_total_score;

#else
		// FIXME: CostCombine is assumed to be sum right now.

		// a final boosting step allowing matched content
		// more weight than unmatched content.

		const size_t n = p_match.size();

		float matched_score = 0.0f;
		float unmatched_score = 0.0f;

		for (size_t i = 0; i < n; i++) {

			const float s = m_t_tokens_pos_weights[i];

			if (p_match[i] < 0) {
				unmatched_score += s;
			} else {
				matched_score += s;
			}
		}

		return p_raw_score / reference_score(matched_score, unmatched_score);
#endif
	}
};

typedef std::shared_ptr<Query> QueryRef;

#endif // __VECTORIAN_QUERY_H__
