template<typename SliceFactory, typename Aligner>
MatcherRef make_matcher(
	const QueryRef &p_query,
	const DocumentRef &p_document,
	const MetricRef &p_metric,
	const SliceFactory &p_factory,
	const Aligner &p_aligner) {

	if (p_query->bidirectional()) {
		return std::make_shared<MatcherImpl<SliceFactory, Aligner, true>>(
			p_query, p_document, p_metric, p_aligner, p_factory);
	} else {
		return std::make_shared<MatcherImpl<SliceFactory, Aligner, false>>(
			p_query, p_document, p_metric, p_aligner, p_factory);
	}
}

template<typename MakeSlice>
class FactoryGenerator {
	const MakeSlice m_make_slice;

public:
	typedef typename std::invoke_result<
		MakeSlice,
		const TokenSpan&,
		const TokenSpan&>::type Slice;

	FactoryGenerator(const MakeSlice &make_slice) :
		m_make_slice(make_slice) {
	}

	SliceFactory<MakeSlice> create(
		const DocumentRef &p_document) const {

		return SliceFactory(m_make_slice);
	}

	FilteredSliceFactory<SliceFactory<MakeSlice>> create_filtered(
		const DocumentRef &p_document,
		const TokenFilter &p_token_filter) const {

		return FilteredSliceFactory(
			create(p_document),
			p_document, p_token_filter);
	}
};