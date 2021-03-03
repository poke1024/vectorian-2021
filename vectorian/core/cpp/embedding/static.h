#ifndef __VECTORIAN_FAST_EMBEDDING_H__
#define __VECTORIAN_FAST_EMBEDDING_H__

#include "common.h"
#include "embedding/embedding.h"
#include "embedding/sim.h"

class VocabularyToEmbedding {
	std::vector<MappedTokenIdArray> m_vocabulary_to_embedding;

public:
	inline VocabularyToEmbedding() {
		m_vocabulary_to_embedding.reserve(2);
	}

	const std::vector<MappedTokenIdArray> &unpack() const {
		return m_vocabulary_to_embedding;
	}

	template<typename F>
	inline void iterate(const F &f) const {
		size_t offset = 0;
		for (const auto &embedding_token_ids : m_vocabulary_to_embedding) {
			f(embedding_token_ids, offset);
			offset += embedding_token_ids.rows();
		}
	}

	inline void append(const std::vector<token_t> &p_mapping) {
		m_vocabulary_to_embedding.push_back(MappedTokenIdArray(
			const_cast<token_t*>(p_mapping.data()), p_mapping.size()));
	}

	inline size_t size() const {
		size_t vocab_size = 0;
		for (const auto &x : m_vocabulary_to_embedding) {
			vocab_size += x.rows();
		}
		return vocab_size;
	}
};

class Needle {
	const std::vector<Token> &m_needle;
	TokenIdArray m_needle_vocabulary_token_ids;
	TokenIdArray m_needle_embedding_token_ids;

public:
	Needle(
		const VocabularyToEmbedding &p_vocabulary_to_embedding,
		const std::vector<Token> &p_needle) :

		m_needle(p_needle) {

		m_needle_vocabulary_token_ids.resize(p_needle.size());
		for (size_t i = 0; i < p_needle.size(); i++) {
			m_needle_vocabulary_token_ids[i] = p_needle[i].id;
		}

		// p_a maps from a Vocabulary corpus token id to an Embedding token id,
		// e.g. 3 in the corpus and 127 in the embedding.

		// p_b are the needle's Vocabulary token ids (not yet mapped to Embedding)

		m_needle_embedding_token_ids.resize(p_needle.size());

		for (size_t i = 0; i < p_needle.size(); i++) {
			const token_t t = m_needle_vocabulary_token_ids[i];
			if (t >= 0) {
				token_t mapped = -1;
				token_t r = t;
				for (const auto &x : p_vocabulary_to_embedding.unpack()) {
					if (r < x.rows()) {
						mapped = x[r];
						break;
					} else {
						r -= x.rows();
					}
				}
				PPK_ASSERT(mapped >= 0);
				m_needle_embedding_token_ids[i] = mapped; // map to Embedding token ids
			} else {
				m_needle_embedding_token_ids[i] = -1;
			}
		}
	}

	const size_t size() const {
		return m_needle.size();
	}

	const TokenIdArray &vocabulary_token_ids() const {
		return m_needle_vocabulary_token_ids;
	}

	const TokenIdArray &embedding_token_ids() const {
		return m_needle_embedding_token_ids;
	}
};

class StaticEmbedding : public Embedding {
	//std::vector<std::string> m_tokens;
	std::unordered_map<std::string, token_t> m_tokens;
	WordVectors m_embeddings;
	std::map<std::string, EmbeddingSimilarityRef> m_similarity_measures;

public:
	EIGEN_MAKE_ALIGNED_OPERATOR_NEW

	StaticEmbedding(
		const std::string &p_name,
		py::object p_table) : Embedding(p_name) {

		const std::shared_ptr<arrow::Table> table = unwrap_table(p_table);

		/*m_tokens.reserve(table->num_rows());
		iterate_strings(table, "token", [this] (size_t i, const std::string &s) {
			PPK_ASSERT(i == m_tokens.size());
			m_tokens.push_back(s);
		});*/

		iterate_strings(table, "token", [this] (size_t i, const std::string &s) {
			m_tokens[s] = static_cast<long>(i);
		});

		std::cout << p_name << ": " << "loaded " << m_tokens.size() << " tokens." << std::endl;

		/*{
			auto tokens = string_column(table, "token");

			if (!tokens) {
				throw std::runtime_error("failed to find parquet table column 'token'\n");
			}

			n = tokens->length();
			m_tokens.reserve(n);
			for (int i = 0; i < n; i++) {
				m_tokens[tokens->GetString(i)] = i;
			}
		}*/
		//const size_t n = m_tokens.length();

		// note: these "raw" tables were already normalized in preprocessing.

		m_embeddings.unmodified.resize(m_tokens.size(), table->num_columns() - 1);
		/*std::cout << "table size: " << table->num_rows() << " x " << table->num_columns() << "\n";
		std::cout << "m_tokens.size(): " << m_tokens.size() << "\n";
		std::cout << std::flush;*/

		PPK_ASSERT(m_tokens.size() == static_cast<size_t>(table->num_rows()));

		try {
			/*printf("loading embedding vectors parquet table.\n");
			fflush(stdout);*/

			for_each_column<arrow::FloatType, float>(table, [this] (size_t i, auto v, size_t offset) {
				PPK_ASSERT(i > 0 && offset + v.size() <= m_tokens.size());
				m_embeddings.unmodified.col(i - 1)(Eigen::seq(offset, v.size())) = v;
			}, 1);
		} catch(...) {
			printf("failed to load embedding vectors parquet table.\n");
			throw;
		}

		m_embeddings.update_normalized();

		//m_similarity_measures = create_similarity_measures(p_name, m_embeddings);
	}

	/*virtual const std::vector<std::string> &tokens() const {
		return m_tokens;
	}*/

	virtual MetricRef create_metric(
		const WordMetricDef &p_metric,
		const py::dict &p_sent_metric_def,
		const VocabularyToEmbedding &p_vocabulary_to_embedding,
		const std::vector<Token> &p_needle) {

		const auto m = std::make_shared<StaticEmbeddingMetric>(
			shared_from_this(),
			p_sent_metric_def);

		const auto s = p_metric.instantiate(m_embeddings);

		const Needle needle(p_vocabulary_to_embedding, p_needle);

		build_similarity_matrix(
			p_vocabulary_to_embedding,
			needle,
			s,
			m->w_similarity());

		//compute_length();

		if (p_sent_metric_def.contains("similarity_falloff")) {
			const float similarity_falloff = p_sent_metric_def["similarity_falloff"].cast<float>();
			m->w_similarity() = m->w_similarity().array().pow(similarity_falloff);
		}

		return m;
	}

	py::dict py_vectors() const {
		return m_embeddings.to_py();
	}

	ssize_t token_to_id(const std::string &p_token) const {
		const auto i = m_tokens.find(p_token);
		if (i != m_tokens.end()) {
			return i->second;
		} else {
			return -1;
		}
	}

	/*float cosine_similarity(const std::string &p_a, const std::string &p_b) const {
		const auto a = m_tokens.find(p_a);
		const auto b = m_tokens.find(p_b);
		if (a != m_tokens.end() && b != m_tokens.end()) {
			return m_embeddings.raw.row(a->second).dot(m_embeddings.raw.row(b->second));
		} else {
			return 0.0f;
		}
	}*/

	/*MatrixXf similarity_matrix(
		const std::string &p_measure,
		TokenIdArray p_s_embedding_ids,
		TokenIdArray p_t_embedding_ids) const {

		auto i = m_similarity_measures.find(p_measure);
		if (i == m_similarity_measures.end()) {
			throw std::runtime_error("unknown similarity measure");
		}

		MatrixXf m;

		i->second->build_matrix(
			m_embeddings, p_s_embedding_ids, p_t_embedding_ids, m);

		return m;
	}*/

	virtual void update_map(
		std::vector<token_t> &p_map,
		const std::vector<std::string> &p_tokens,
		const size_t p_offset) const {

		const size_t i0 = p_map.size();
		const size_t i1 = p_tokens.size();
		PPK_ASSERT(i0 <= i1);
		if (i0 == i1) {
			return;
		}
		p_map.resize(i1);

		for (size_t i = i0; i < i1; i++) {
			const auto it = m_tokens.find(p_tokens[i]);
			if (it != m_tokens.end()) {
				p_map[i] = it->second;
			} else {
				p_map[i] = -1;
			}
		}
	}

	size_t n_tokens() const {
		return m_embeddings.unmodified.rows();
	}

	py::list measures() const {
		py::list names;
		for (auto i : m_similarity_measures) {
			names.append(py::str(i.first));
		}
		return names;
	}

private:
	void compute_magnitudes() {
		/*for (size_t j = 0; j < p_needle.size(); j++) {
			const size_t k = needle_embedding_token_ids[j];
			r_length_t(j) = m_embeddings.unmodified[k].norm();
		}*/

		/*size_t offset = 0;
		for (const auto &x : p_vocabulary_to_embedding) {
			const auto n = x.rows();
			for (size_t i = 0; i < n; i++) {
				r_length_s(offset + i) = m_embeddings.unmodified[x(i)].norm();
			}
			offset += n;
		}
		}*/

	}

	void build_similarity_matrix(
		const VocabularyToEmbedding &p_vocabulary_to_embedding,
		const Needle &p_needle,
		const EmbeddingSimilarityRef &p_embedding_similarity,
		MatrixXf &r_matrix) const {

		py::gil_scoped_release release;

		const size_t vocab_size = p_vocabulary_to_embedding.size();
		//std::cout << "resizing matrix " << vocab_size << " x " << needle_embedding_token_ids.rows() << "\n";
		r_matrix.resize(vocab_size, p_needle.embedding_token_ids().rows());

		p_vocabulary_to_embedding.iterate([&] (const auto &embedding_token_ids, size_t offset) {
			p_embedding_similarity->fill_matrix(
				m_embeddings,
				embedding_token_ids,
				p_needle.embedding_token_ids(),
				offset,
				0,
				r_matrix);
		});

		for (size_t j = 0; j < p_needle.size(); j++) { // for each token in needle

			// since the j-th needle token is a specific vocabulary token, we always
			// set that specific vocabulary token similarity to 1 (regardless of the
			// embedding distance).
			const auto k = p_needle.vocabulary_token_ids()[j];
			if (k >= 0) {
				r_matrix(k, j) = 1.0f;
			}
		}

	}
};

typedef std::shared_ptr<StaticEmbedding> StaticEmbeddingRef;

#endif // __VECTORIAN_FAST_EMBEDDING_H__
