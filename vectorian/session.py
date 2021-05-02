import vectorian.core as core
import vectorian.normalize as normalize
import logging
import collections
import time
import numpy as np

from cached_property import cached_property
from functools import lru_cache
from vectorian.render.render import Renderer
from vectorian.render.excerpt import ExcerptRenderer
from vectorian.render.location import LocationFormatter
from vectorian.metrics import CosineSimilarity, TokenSimilarity, AlignmentSimilarity, PartitionSimilarity
from vectorian.embeddings import VectorsCache, Vectors


class Result:
	def __init__(self, index, matches, duration):
		self._index = index
		self._matches = matches
		self._duration = duration

	@property
	def index(self):
		return self._index

	@property
	def matches(self):
		return self._matches

	def __len__(self):
		return len(self._matches)

	def __iter__(self):
		return self._matches

	def __getitem__(self, i):
		return self._matches[i]

	def to_json(self, context_size=10):
		return [m.to_json(context_size) for m in self._matches]

	def limit_to(self, n):
		return type(self)(self._matches[:n])

	@property
	def duration(self):
		return self._duration


class Collection:
	def __init__(self, session, vocab, corpus):
		self._vocab = vocab
		self._docs = []
		for i, doc in enumerate(corpus):
			self._docs.append(doc.prepare(session, i))

	@property
	def documents(self):
		return self._docs

	@lru_cache(16)
	def max_len(self, level, window_size):
		return max([doc.compiled.max_len(level, window_size) for doc in self._docs])


Slice = collections.namedtuple('Slice', ['level', 'start', 'end'])


class Partition:
	def __init__(self, session, level, window_size, window_step):
		self._session = session
		self._level = level
		self._window_size = window_size
		self._window_step = window_step

	@property
	def contiguous(self):
		return self._window_step <= self._window_size

	@property
	def session(self):
		return self._session

	@property
	def level(self):
		return self._level

	@property
	def window_size(self):
		return self._window_size

	@property
	def window_step(self):
		return self._window_step

	def to_args(self):
		return {
			'level': self._level,
			'window_size': self._window_size,
			'window_step': self._window_step
		}

	@property
	def cache_key(self):
		return self._level, self._window_size, self._window_step

	@cached_property
	def freq(self):
		freq = core.Frequencies(self._session.vocab)
		strategy = core.SliceStrategy(self.to_args())
		for doc in self._session.documents:
			freq.add(doc.compiled, strategy)
		return freq

	def max_len(self):
		return self._session.max_len(self._level, self._window_size)

	def index(self, metric, nlp=None, **kwargs):
		if not isinstance(metric, PartitionSimilarity):
			raise TypeError(metric)

		if nlp:
			kwargs = kwargs.copy()
			kwargs['nlp'] = nlp

		return metric.create_index(self, **kwargs)

	def slice_id_to_slice(self, slice_id):
		return Slice(self._level, self._window_step * slice_id, self._window_size)


SessionEmbedding = collections.namedtuple(
	"SessionEmbedding", ["factory", "instance"])


class Session:
	def __init__(self, docs, embeddings=None, normalizers=None):
		if embeddings is None:
			embeddings = []

		if normalizers == "default":
			normalizers = normalize.default_normalizers()

		self._embedding_manager = core.EmbeddingManager()

		if any(e.is_static for e in embeddings) and not normalizers:
			logging.warning("got static embeddings but no normalizers.")

		if normalizers is None:
			normalizers = {}
		self._normalizers = normalize.normalizer_dict(normalizers)

		self._embeddings = tuple(embeddings)

		for embedding in self._embeddings:
			if embedding.is_contextual:
				for doc in docs:
					if not doc.has_contextual_embedding(embedding.name):
						raise RuntimeError(f"doc {doc.unique_id} misses contextual embedding {embedding.name}")

		self._embedding_instances = {}
		for embedding in self._embeddings:
			instance = embedding.create_instance(self)
			self._embedding_instances[instance.name] = SessionEmbedding(
				factory=embedding,
				instance=instance)
			self._embedding_manager.add_embedding(instance)

		self._vocab = core.Vocabulary(self._embedding_manager)

		self._collection = Collection(
			self, self._vocab, docs)

		self._vocab.compile_embeddings()  # i.e. static embeddings
		self._embedding_manager.compile_contextual()

		self._vectors_cache = VectorsCache()

	@property
	def vocab(self):
		return self._vocab

	def default_metric(self):
		embedding = self._embeddings[0]
		return AlignmentSimilarity(
			TokenSimilarity(
				embedding, CosineSimilarity()))

	@cached_property
	def documents(self):
		return self._collection.documents

	@cached_property
	def c_documents(self):
		return [x.compiled for x in self.documents]

	@property
	def embeddings(self):
		return self._embedding_instances

	def to_embedding_instance(self, embedding):
		return self._embedding_instances[embedding.name].instance

	@property
	def vectors_cache(self):
		return self._vectors_cache

	@lru_cache(16)
	def max_len(self, level, window_size):
		return self._collection.max_len(level, window_size)

	def make_result(self, *args, **kwargs):
		return Result(*args, **kwargs)

	def on_progress(self, task, disable_progress=False):
		return task(None)

	def partition(self, level, window_size=1, window_step=None):
		if window_step is None:
			window_step = window_size
		return Partition(self, level, window_size, window_step)

	@property
	def normalizers(self):
		return self._normalizers

	def normalizer(self, stage):
		return self._normalizers[stage]

	def word_vec(self, embedding, word):
		return self.to_embedding_instance(embedding).word_vec(word)

	def similarity(self, token_sim, a, b):
		out = np.zeros((1, 1), dtype=np.float32)
		if token_sim.is_modifier:
			x = np.zeros((len(token_sim.operands), 1), dtype=np.float32)
			for i, op in enumerate(token_sim.operands):
				x[i] = self.similarity(op, a, b)
			token_sim(x, out)
		else:
			ei = self.to_embedding_instance(
				token_sim.embedding)
			va = Vectors([ei.word_vec(a)])
			vb = Vectors([ei.word_vec(b)])
			token_sim.metric(va, vb, out)
		return out[0, 0]


class LabResult(Result):
	def __init__(self, index, matches, duration, renderers, location_formatter):
		super().__init__(index, matches, duration)
		self._renderers = renderers
		self._location_formatter = location_formatter
		self._annotate = {}

	def _render(self, r):
		# see https://ipython.readthedocs.io/en/stable/api/generated/IPython.display.html#IPython.display.display
		return r.to_html(self._matches)

	def format(self, render_spec):
		renderers = []
		if isinstance(render_spec, (list, tuple)):
			renderers = render_spec
		else:
			def load_excerpt_renderer():
				from vectorian.render.excerpt import ExcerptRenderer
				return ExcerptRenderer

			def load_flow_renderer():
				from vectorian.render.sankey import FlowRenderer
				return FlowRenderer

			def load_matrix_renderer():
				from vectorian.render.matrix import MatrixRenderer
				return MatrixRenderer

			lookup = {
				'excerpt': load_excerpt_renderer,
				'flow': load_flow_renderer,
				'matrix': load_matrix_renderer
			}

			klass = None
			args = []
			for render_desc in render_spec.split(","):
				for i, part in enumerate(render_desc.split()):
					part = part.strip()
					if i == 0:
						klass = lookup[part]()
						args = []
					else:
						if part.startswith("+"):
							args.append(part[1:].strip())
						else:
							raise ValueError(part)

				if klass is not None:
					renderers.append(klass(*args))
					klass = None

			if klass is not None:
				renderers.append(klass(*args))
				klass = None

		return LabResult(
			self.index,
			self._matches,
			self._duration,
			renderers,
			self._location_formatter)

	def _repr_html_(self):
		return self._render(Renderer(
			self._renderers,
			self._location_formatter,
			annotate=self._annotate))


class LabSession(Session):
	def __init__(self, *args, location_formatter=None, **kwargs):
		super().__init__(*args, **kwargs)
		self._location_formatter = location_formatter or LocationFormatter()
		self._progress = None
		self._last_progress_update = None

	def interact(self, nlp):
		from vectorian.interact import InteractiveQuery

		logger = logging.getLogger()
		logger.setLevel(logging.WARNING)

		q = InteractiveQuery(self, nlp)
		return q.widget

	def make_result(self, *args, **kwargs):
		return LabResult(
			*args, **kwargs,
			renderers=[ExcerptRenderer()],
			location_formatter=self._location_formatter)

	def _create_progress(self):
		import ipywidgets as widgets
		from IPython.display import display

		if self._progress is not None:
			return

		self._progress = widgets.FloatProgress(
			value=0, min=0, max=1, description="",
			layout=widgets.Layout(width="100%"))

		display(self._progress)

	def _update_progress(self, t):
		update_delay = 0.5

		now = time.time()
		if now - self._last_progress_update < update_delay:
			return

		self._create_progress()

		new_value = self._progress.max * t
		self._progress.value = new_value
		self._last_progress_update = now

	def on_progress(self, task, disable_progress=False):
		self._last_progress_update = time.time()

		try:
			if disable_progress:
				result = task(None)
			else:
				result = task(self._update_progress)
		finally:
			if self._progress:
				self._progress.close()
				self._progress = None

		return result

