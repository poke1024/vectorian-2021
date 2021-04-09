import numpy as np
import io


class GapCost:
	def costs(self, n):
		raise NotImplementedError

	def _plot(self, ax, n):
		from matplotlib.ticker import MaxNLocator
		c = self.costs(n)
		ax.plot(c)
		ax.set_xlabel('gap length')
		ax.set_ylabel('cost')
		ax.set_ylim(-0.1, 1)
		ax.xaxis.set_major_locator(MaxNLocator(integer=True))
		ax.grid()

	def plot(self, n):
		import matplotlib.pyplot as plt
		fig, ax = plt.subplots(1, 1, figsize=(12, 3))
		self._plot(ax, n)
		fig.tight_layout()
		fig.show()

	def plot_to_image(self, fig, ax, n, format='png'):
		self._plot(ax, n)
		buf = io.BytesIO()
		fig.tight_layout()
		fig.savefig(buf, format=format)
		buf.seek(0)
		data = buf.getvalue()
		buf.close()
		return data

	def _ipython_display_(self):
		# see https://ipython.readthedocs.io/en/stable/config/integrating.html
		self.plot(50)


class ConstantGapCost(GapCost):
	def __init__(self, cost):
		self._cost = cost

	def to_description(self):
		return f'ConstantGapCost({self._cost})'

	def costs(self, n):
		c = np.empty((n,), dtype=np.float32)
		c.fill(self._cost)
		c[0] = 0
		return c


class LinearGapCost(GapCost):
	def __init__(self, step, start=None):
		self._step = step
		self._start = step if start is None else start

	def to_scalar(self):
		assert self._start == self._step
		return self._step

	def to_description(self):
		return f'LinearGapCost({self._step}, {self._start})'

	def costs(self, n):
		c = np.empty((n,), dtype=np.float32)
		c[0] = 0
		x = self._start
		for i in range(1, n):
			c[i] = x
			x += self._step
		return c


class ExponentialGapCost(GapCost):
	def __init__(self, cutoff):
		self._cutoff = cutoff

	def to_description(self):
		return f'ExponentialGapCost({self._cutoff})'

	def costs(self, n):
		c = np.empty((n,), dtype=np.float32)
		if self._cutoff > 0:
			for i in range(n):
				c[i] = 1 - (2 ** -(i / self._cutoff))
		else:
			c.fill(1)
			c[0] = 0
		return c


class CustomGapCost(GapCost):
	def __init__(self, costs_fn):
		self._costs_fn = costs_fn

	def to_description(self):
		return f'CustomGapCost({self._costs_fn})'

	def costs(self, n):
		c = np.empty((n,), dtype=np.float32)
		c[0] = 0
		for i in range(1, n):
			c[i] = self._costs_fn(i)
		return c


class AlignmentAlgorithm:
	def to_description(self, partition):
		raise NotImplementedError()

	def to_args(self, partition):
		raise NotImplementedError()


class NeedlemanWunsch(AlignmentAlgorithm):
	def __init__(self, gap: float = 0):
		self._gap = gap

	def to_description(self, partition):
		return {
			'NeedlemanWunsch': {
				'gap': self._gap
			}
		}

	def to_args(self, partition):
		return {
			'algorithm': 'needleman-wunsch',
			'gap': self._gap
		}


class SmithWaterman(AlignmentAlgorithm):
	def __init__(self, gap: float = 0, zero: float = 0.5):
		self._gap = gap
		self._zero = zero

	def to_description(self, partition):
		return {
			'WatermanSmithBeyer': {
				'gap': self._gap,
				'zero': self._zero
			}
		}

	def to_args(self, partition):
		return {
			'algorithm': 'smith-waterman',
			'gap': self._gap,
			'zero': self._zero
		}


class WatermanSmithBeyer(AlignmentAlgorithm):
	def __init__(self, gap: GapCost = None, zero: float = 0.5):
		if gap is None:
			gap = ConstantGapCost(0)
		self._gap = gap
		self._zero = zero

	def to_description(self, partition):
		return {
			'WatermanSmithBeyer': {
				'gap': self._gap.to_description(),
				'zero': self._zero
			}
		}

	def to_args(self, partition):
		costs = self._gap.costs(partition.max_len())

		return {
			'algorithm': 'waterman-smith-beyer',
			'gap': np.clip(costs, 0, 1),
			'zero': self._zero
		}


class WordMoversDistance(AlignmentAlgorithm):
	@staticmethod
	def wmd(variant='kusner', **kwargs):
		if variant == 'kusner':
			return WordMoversDistance(False, False, False, True, **kwargs)
		elif variant == 'vectorian':
			return WordMoversDistance(False, False, False, False, **kwargs)
		else:
			raise ValueError(variant)

	@staticmethod
	def rwmd(variant, **kwargs):
		if variant == 'kusner':
			return WordMoversDistance(True, True, True, True, **kwargs)
		elif variant == 'jablonsky':
			return WordMoversDistance(True, False, True, True, **kwargs)
		elif variant == 'vectorian':
			return WordMoversDistance(True, True, False, False, **kwargs)
		else:
			raise ValueError(variant)

	def __init__(self, relaxed=True, injective=True, symmetric=False, normalize_bow=False, extra_mass_penalty=-1):
		self._options = {
			'relaxed': relaxed,
			'injective': injective,
			'normalize_bow': normalize_bow,
			'symmetric': symmetric,
			'extra_mass_penalty': extra_mass_penalty
		}

	def to_description(self, partition):
		return {
			'WordMoversDistance': self._options
		}

	def to_args(self, partition):
		return {
			'algorithm': 'word-movers-distance',
			'relaxed': self._options['relaxed'],
			'injective': self._options['injective'],
			'symmetric': self._options['symmetric'],
			'normalize_bow': self._options['normalize_bow'],
			'extra_mass_penalty': self._options['extra_mass_penalty']
		}


class WordRotatorsDistance(AlignmentAlgorithm):
	def __init__(self, normalize_magnitudes=True, extra_mass_penalty=-1):
		self._normalize_magnitudes = normalize_magnitudes
		self._extra_mass_penalty = extra_mass_penalty

	def to_description(self, partition):
		return {
			'WordRotatorsDistance': {
				'normalize_magnitudes': self._normalize_magnitudes,
				'extra_mass_penalty': self._extra_mass_penalty
			}
		}

	def to_args(self, partition):
		return {
			'algorithm': 'word-rotators-distance',
			'normalize_magnitudes': self._normalize_magnitudes,
			'extra_mass_penalty': self._extra_mass_penalty
		}
