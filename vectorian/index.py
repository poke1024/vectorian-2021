import vectorian.core as core
import spacy
import multiprocessing
import multiprocessing.pool
import time
import numpy as np
import bisect

from collections import namedtuple
from tqdm import tqdm
from vectorian.corpus.document import TokenTable, extract_token_str


class Query:
	def __init__(self, index, vocab, doc, options):
		self._index = index
		self._vocab = vocab
		self._doc = doc
		self._options = options

	@property
	def text(self):
		return self._doc.text

	@property
	def options(self):
		return self._options

	def _filter(self, tokens, name, k):
		f = self._options.get(name, None)
		if f:
			s = set(f)
			return [t for t in tokens if t[k] not in s]
		else:
			return tokens

	def to_core(self):
		tokens = self._doc.to_json()["tokens"]
		tokens = self._filter(tokens, 'pos_filter', 'pos')
		tokens = self._filter(tokens, 'tag_filter', 'tag')

		token_table = TokenTable()
		text = self._doc.text
		token_table.extend(text, {'start': 0, 'end': len(text)}, tokens)

		token_table_pa = token_table.to_arrow()
		return core.Query(
			self._vocab,
			token_table_pa,
			extract_token_str(
				token_table_pa,
				self.text,
				self._index.session.options.token_normalizer),
			**self._options)


Region = namedtuple('Region', [
	's', 'match', 'gap_penalty'])


TokenMatch = namedtuple('TokenMatch', [
	't', 'pos_s', 'pos_t', 'similarity', 'weight', 'metric'])


class Match:
	def __init__(self, query, document, sentence, score, metric=None, omitted=None, regions=None, level="word"):
		self._query = query
		self._document = document
		self._sentence = sentence
		self._score = score
		self._metric = metric or ""
		self._omitted = omitted or []
		self._regions = regions or []
		self._level = level

	@staticmethod
	def from_core(session, query, c_match):
		document = session.documents[c_match.document.id]

		s_text = document.text
		t_text = query.text

		regions = []
		for r in c_match.regions:
			if r.matched:
				regions.append(Region(
					s=s_text[slice(*r.s)],
					match=TokenMatch(
						t=t_text[slice(*r.t)],
						pos_s=r.pos_s.decode('utf-8'),
						pos_t=r.pos_t.decode('utf-8'),
						similarity=r.similarity,
						weight=r.weight,
						metric=r.metric.decode('utf-8')),
					gap_penalty=r.mismatch_penalty))
			else:
				regions.append(Region(
					s=s_text[slice(*r.s)],
					match=None,
					gap_penalty=r.mismatch_penalty))

		omitted = [s_text[slice(*s)] for s in c_match.omitted]

		return Match(
			query, document, c_match.sentence, c_match.score,
			c_match.metric, omitted, regions)

	@property
	def document(self):
		return self._document

	@property
	def sentence(self):
		return self._sentence

	@property
	def score(self):
		return self._score

	@property
	def metric(self):
		return self._metric

	@property
	def omitted(self):
		return self._omitted

	@property
	def regions(self):
		return self._regions

	@property
	def level(self):
		return self._level

	def to_json(self, session):
		regions = []
		doc = self.document
		sentence_info = doc.sentence_info(self.sentence)

		for r in self.regions:
			s = r.s
			rm = r.match
			if rm:
				t = rm.t
				regions.append(dict(
					s=s,
					t=t,
					similarity=rm.similarity,
					weight=rm.weight,
					pos_s=rm.pos_s,
					pos_t=rm.pos_t,
					metric=rm.metric))
			else:
				regions.append(dict(s=s, gap_penalty=r.gap_penalty))

		metadata = doc.metadata
		loc = session.location_formatter(doc, sentence_info)
		if loc:
			speaker, loc_desc = loc
		else:
			speaker = ""
			loc_desc = ""

		return dict(
			debug=dict(document=metadata["unique_id"], sentence=self.sentence),
			score=self.score,
			metric=self.metric,
			location=dict(
				speaker=speaker,
				author=metadata["author"],
				title=metadata["title"],
				location=loc_desc
			),
			regions=regions,
			omitted=self.omitted,
			level=self.level)


class Index:
	def __init__(self, session, metric):
		self._session = session
		self._metric = metric

	@property
	def session(self):
		return self._session

	def find(
		self, doc: spacy.tokens.doc.Doc,
		n=10, min_score=0.2,
		options: dict = dict()):

		if not isinstance(doc, spacy.tokens.doc.Doc):
			raise TypeError("please specify a spaCy document as query")

		metric_args = self._metric.to_args(self._session)

		options = options.copy()
		if metric_args:
			options["metric"] = metric_args
		options["max_matches"] = n
		options["min_score"] = min_score

		start_time = time.time()

		query = Query(self, self._session.vocab, doc, options)
		result_class, matches = self._session.run_query(self._find, query)

		return result_class(
			self,
			matches,
			duration=time.time() - start_time)


class BruteForceIndex(Index):
	def _find(self, query, n_threads=None, progress=None):
		c_query = query.to_core()

		def find_in_doc(x):
			return x, x.find(c_query)

		docs = self._session.c_documents

		total = sum([x.n_tokens for x in docs])
		done = 0

		if n_threads is None:
			n_threads = min(len(docs), multiprocessing.cpu_count())

		results = None
		with multiprocessing.pool.ThreadPool(processes=n_threads) as pool:
			for doc, r in pool.imap_unordered(find_in_doc, docs):
				if results is None:
					results = r
				else:
					results.extend(r)
				done += doc.n_tokens
				if progress:
					progress(done / total)

		return [Match.from_core(self._session, query, m) for m in results.best_n(-1)]


def chunks(x, n):
	for i in range(0, len(x), n):
		yield x[i:i + n]


class SentenceEmbeddingIndex(Index):
	def __init__(self, session, metric, encoder):
		super().__init__(session, metric)

		self._encoder = encoder
		self._metric = metric

		corpus_vec = []
		doc_starts = [0]

		chunk_size = 50
		n_sentences = sum(doc.n_sentences for doc in session.documents)

		with tqdm(desc="Encoding", total=n_sentences) as pbar:
			for i, doc in enumerate(session.documents):
				sentences = list(doc.sentences)
				for chunk in chunks(sentences, chunk_size):
					doc_vec = encoder(chunk)
					corpus_vec.append(doc_vec)
					pbar.update(len(chunk))
				doc_starts.append(len(sentences))

		corpus_vec = np.vstack(corpus_vec)
		corpus_vec /= np.linalg.norm(corpus_vec, axis=1, keepdims=True)

		self._doc_starts = np.cumsum(np.array(doc_starts, dtype=np.int32))

		try:
			import faiss
		except ImportError:
			raise

		# https://github.com/facebookresearch/faiss/wiki/Faiss-indexes
		# https://github.com/facebookresearch/faiss/wiki/Guidelines-to-choose-an-index

		pca_dim = 128
		if corpus_vec.shape[0] < pca_dim:
			pca_dim = None

		# https://github.com/facebookresearch/faiss/wiki/The-index-factory
		n_dims = corpus_vec.shape[-1]
		index = faiss.index_factory(n_dims, "Flat", faiss.METRIC_INNER_PRODUCT)
		#index = faiss.index_factory(n_dims, "PCA128,LSH", faiss.METRIC_INNER_PRODUCT)
		#index = faiss.index_factory(n_dims, "LSH", faiss.METRIC_INNER_PRODUCT)
		index.train(corpus_vec)
		index.add(corpus_vec)

		self._index = index

	def save(self, path):
		pass

	def _find(self, query, progress=None):
		query_vec = self._encoder([query.text])
		query_vec /= np.linalg.norm(query_vec)

		distance, index = self._index.search(
			query_vec, query.options["max_matches"])

		matches = []
		for d, i in zip(distance[0], index[0]):
			if i < 0:
				break

			doc_index = bisect.bisect_left(self._doc_starts, i)
			if doc_index > 0 and self._doc_starts[doc_index] > i:
				doc_index -= 1
			sent_index = i - self._doc_starts[doc_index]

			#print(i, doc_index, sent_index, self._doc_starts)

			doc = self._session.documents[doc_index]
			score = (d + 1) * 0.5

			#print(c_doc, sentence_id)
			#print(c_doc.sentence(sentence_id))
			#print(score, d)

			sent_text = doc.sentence(sent_index)
			#print(sent_text, len(sent_text), c_doc.sentence_info(sent_index))

			regions = [Region(
				s=sent_text.strip(),
				match=None, gap_penalty=0)]

			matches.append(Match(
				query,
				doc,
				sent_index,
				score,
				self._metric.name,
				regions=regions,
				level="sentence"
			))

		return matches
