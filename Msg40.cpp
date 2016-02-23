#include "gb-include.h"

#include "Msg40.h"
#include "Stats.h"        // for timing and graphing time to get all summaries
#include "Collectiondb.h"
#include "LanguageIdentifier.h"
#include "sort.h"
#include "matches2.h"
#include "XmlDoc.h" // computeSimilarity()
#include "Speller.h"
#include "Wiki.h"
#include "HttpServer.h"
#include "PageResults.h"
#include "HashTable.h"

// increasing this doesn't seem to improve performance any on a single
// node cluster....
#define MAX_OUTSTANDING_MSG20S 200

bool printHttpMime ( class State0 *st ) ;

static void gotDocIdsWrapper             ( void *state );
static bool gotSummaryWrapper            ( void *state );

// here's the GIGABIT knobs:

// sample radius in chars around each query term    : 600  (line  212)
// max sample size, all excerpts, per document      : 100k (line  213)
// map from distance to query term in words to score:      (line  855)
// map from popularity to score weight              :      (lines 950 et al)
// the comments above are way out of date (aac, Jan 2008)
// 
// QPOP multiplier params
#define QPOP_ZONE_0          10
#define QPOP_ZONE_1          30
#define QPOP_ZONE_2          80
#define QPOP_ZONE_3          100
#define QPOP_ZONE_4          300
#define QPOP_MULT_0          10
#define QPOP_MULT_1          8
#define QPOP_MULT_2          6
#define QPOP_MULT_3          4
#define QPOP_MULT_4          2
// QTR scoring params
#define MAX_SCORE_MULTIPLIER 3000  // orig: 3000
#define ALT_MAX_SCORE        12000 // orig: 12000
#define ALT_START_SCORE      1000
#define QTR_ZONE_0           4
#define QTR_ZONE_1           8
#define QTR_ZONE_2           12
#define QTR_ZONE_3           20
#define QTR_BONUS_0          1000
#define QTR_BONUS_1          800
#define QTR_BONUS_2          500
#define QTR_BONUS_3          200
#define QTR_BONUS_CW         1
#define MULTIPLE_HIT_BOOST   1000 // orig: 1000
// gigabit phrase scoring params
#define FWC_PENALTY          500   // penalty for begining with common word
#define POP_ZONE_0           10 // 0.00001
#define POP_ZONE_1           30 //0.0001
#define POP_ZONE_2           80 // 0.001
#define POP_ZONE_3           300 // 0.01
#define POP_BOOST_0          4.0
#define POP_BOOST_1          3.0
#define POP_BOOST_2          2.0
#define POP_BOOST_3          1.0
#define POP_BOOST_4          0.1



bool isSubDom(char *s , int32_t len);

Msg40::Msg40() {
	m_calledFacets = false;
	m_doneWithLookup = false;
	m_socketHadError = 0;
	m_buf           = NULL;
	m_buf2          = NULL;
	m_cachedResults = false;
	m_msg20         = NULL;
	m_numMsg20s     = 0;
	m_msg20StartBuf = NULL;
	m_numToFree     = 0;
	// new stuff for streaming results:
	m_hadPrintError = false;
	m_numPrinted    = 0;
	m_printedHeader = false;
	m_printedTail   = false;
	m_sendsOut      = 0;
	m_sendsIn       = 0;
	m_printi        = 0;
	m_numDisplayed  = 0;
	m_numPrintedSoFar = 0;
	m_lastChunk     = false;
	m_didSummarySkip = false;
	m_omitCount      = 0;
	m_printCount = 0;
	m_numCollsToSearch = 0;
	m_numMsg20sIn = 0;
	m_numMsg20sOut = 0;
}

#define MAX2 50

void Msg40::resetBuf2 ( ) {
	// remember num to free in reset() function
	char *p = m_msg20StartBuf;
	// msg20 destructors
	for ( int32_t i = 0 ; i < m_numToFree ; i++ ) {
		// cast it
		Msg20 *m = (Msg20 *)p;
		// free its stuff
		m->destructor();
		// advance
		p += sizeof(Msg20);
	}
	// now free the msg20 ptrs and buffer space
	if ( m_buf2 ) mfree ( m_buf2 , m_bufMaxSize2 , "Msg40b" );
	m_buf2 = NULL;


	// make a safebuf of 50 of them if we haven't yet
	if ( m_unusedBuf.length() <= 0 ) return;
	Msg20 *ma = (Msg20 *)m_unusedBuf.getBufStart();
	for ( int32_t i = 0 ; i < (int32_t)MAX2 ; i++ ) ma[i].destructor();
}

Msg40::~Msg40() {
	// free tmp msg3as now
	for ( int32_t i = 0 ; i < m_numCollsToSearch ; i++ ) {
		if ( ! m_msg3aPtrs[i] ) continue;
		if ( m_msg3aPtrs[i] == &m_msg3a ) continue;
		mdelete ( m_msg3aPtrs[i] , sizeof(Msg3a), "tmsg3a");
		delete  ( m_msg3aPtrs[i] );
		m_msg3aPtrs[i] = NULL;
	}
	if ( m_buf  ) mfree ( m_buf  , m_bufMaxSize  , "Msg40" );
	m_buf  = NULL;
	resetBuf2();
}

bool Msg40::registerHandler ( ) {
        return true;
}

// . returns false if blocked, true otherwise
// . sets g_errno on error
// . uses Msg3a to get docIds
// . uses many msg20s to get title/summary/url/docLen for each docId
bool Msg40::getResults ( SearchInput *si      ,
			 bool         forward ,
			 void        *state   ,
			 void   (* callback) ( void *state ) ) {

	m_omitCount = 0;

	// warning
	//if ( ! si->m_coll2 ) log(LOG_LOGIC,"net: NULL collection. msg40.");
	if ( si->m_collnumBuf.length() < (int32_t)sizeof(collnum_t) )
		log(LOG_LOGIC,"net: NULL collection. msg40.");


	m_lastProcessedi = -1;
	m_didSummarySkip = false;

	m_si             = si;
	m_state          = state;
	m_callback       = callback;
	m_msg3aRecallCnt = 0;
	// we haven't allocated any Msg20s yet
	m_numMsg20s      = 0;
	// reset our error keeper
	m_errno = 0;
	// we need this info for caching as well
	//m_numGigabitInfos = 0;

	//just getfrom searchinput
	//....	m_catId = hr->getLong("catid",0);m_si->m_catId;

 	m_postQueryRerank.set1( this, si );

	// take search parms i guess from first collnum
	collnum_t *cp = (collnum_t *)m_si->m_collnumBuf.getBufStart();

	// get the collection rec
	CollectionRec *cr =g_collectiondb.getRec( cp[0] );

	// g_errno should be set if not found
	if ( ! cr ) { g_errno = ENOCOLLREC; return true; }

	// save that
	m_firstCollnum = cr->m_collnum;

	// what is our max docids ceiling?
	//m_maxDocIdsToCompute = cr->m_maxDocIdsToCompute;
	// topic similarity cutoff
	m_topicSimilarCutoff = cr->m_topicSimilarCutoffDefault ;

	// reset this for family filter
	m_queryCensored = false;
	m_filterStats[CR_DIRTY]	= 0;  //m_numCensored = 0;

	// . reset these
	// . Next X Results links? yes or no?
	m_moreToCome = false;
	// set this to zero -- assume not in cache
	m_cachedTime = 0;
	// assume we are not taken from the serp cache
	m_cachedResults  = false;

	// bail now if 0 requested!
	// crap then we don't stream anything if in streaming mode.
	if ( m_si->m_docsWanted == 0 ) {
		log("msg40: setting streamresults to false. n=0.");
		m_si->m_streamResults = false;
		return true;
	}

	// or if no query terms
	if ( m_si->m_q.m_numTerms <= 0 ) {
		log("msg40: setting streamresults to false. numTerms=0.");
		m_si->m_streamResults = false;
		return true;
	}

	// . do this now in case results were cached.
	// . set SearchInput class instance, m_si
	// . has all the input that we need to get the search results just
	//   the way the caller wants them
	//m_msg1a.setSearchInput(m_si);

	// how many docids do we need to get?
	int32_t get = m_si->m_docsWanted + m_si->m_firstResultNum ;
	// we get one extra for so we can set m_moreToFollow so we know
	// if more docids can be gotten (i.e. show a "Next 10" link)
	get++;

	// ok, need some sane limit though to prevent malloc from 
	// trying to get 7800003 docids and going ENOMEM
	if ( get > MAXDOCIDSTOCOMPUTE ) {
		log("msg40: asking for too many docids. reducing to %"INT32"",
		    (int32_t)MAXDOCIDSTOCOMPUTE);
		get = MAXDOCIDSTOCOMPUTE;
	}
	// this is how many visible results we need, after filtering/clustering
	m_docsToGetVisible = get;

	// . get a little more since this usually doesn't remove many docIds
	// . deduping is now done in Msg40.cpp once the summaries are gotten
	if ( m_si->m_doDupContentRemoval ) get = (get*120LL)/100LL;

	// . ALWAYS get at least this many
	// . this allows Msg3a to allow higher scoring docids in tier #1 to
	//   outrank lower-scoring docids in tier #0, even if such docids have
	//   all the query terms explicitly. and we can guarantee consistency
	//   as int32_t as we only allow for this outranking within the first
	//   MIN_DOCS_TO_GET docids.
	if ( get < MIN_DOCS_TO_GET ) get = MIN_DOCS_TO_GET;
	// this is how many docids to get total, assuming that some will be
	// filtered out for being dups, etc. and that we will have at least
	// m_docsToGetVisible leftover that are unfiltered and visible. so
	// we tell each msg39 split to get more docids than we actually want
	// in anticipation some will be filtered out in this class.
	m_docsToGet = get;

	// debug msg
	if ( m_si->m_debug ) 
		logf(LOG_DEBUG,"query: msg40 mapped %"INT32" wanted to %"INT32" to get",
		     m_docsToGetVisible,m_docsToGet );

	// let's try using msg 0xfd like Proxy.cpp uses to forward an http
	// request! then we just need specify the ip of the proxy and we
	// do not need hosts2.conf!
	if ( forward ) { char *xx=NULL;*xx=0; }

	// time the cache lookup
	if ( g_conf.m_logTimingQuery || m_si->m_debug || g_conf.m_logDebugQuery) 
		m_startTime = gettimeofdayInMilliseconds();

	// keep going
	bool status = prepareToGetDocIds ( );

	if ( status && m_si->m_streamResults ) {
		log("msg40: setting streamresults to false. "
		    "prepare did not block.");
		m_si->m_streamResults = false;
	}

	return status;
}

bool Msg40::gotCacheReply ( ) {
	// if not found, get the result the hard way
	if ( ! m_msg17.wasFound() ) return prepareToGetDocIds ( );
	// otherwise, get the deserialized stuff
	int32_t nb = deserialize(m_cachePtr, m_cacheSize);
	if ( nb <= 0 ) {
		log ("query: Deserialization of cached search results "
		     "page failed." );
		// free m_buf!
		if ( m_buf ) 
			mfree ( m_buf , m_bufMaxSize , "deserializeMsg40");
		// get results the hard way!
		return prepareToGetDocIds ( );
	}
	// log the time it took for cache lookup
	if ( g_conf.m_logTimingQuery ) {
		int64_t now  = gettimeofdayInMilliseconds();
		int64_t took = now - m_startTime;
		log(LOG_TIMING,
		    "query: [%"PTRFMT"] found in cache. "
		    "lookup took %"INT64" ms.",(PTRTYPE)this,took);
	}
	m_cachedTime = m_msg17.getCachedTime();
	m_cachedResults = true;
	// if it was found, we return true, m_cachedTime should be set
	return true;
}

bool Msg40::prepareToGetDocIds ( ) {

	// log the time it took for cache lookup
	if ( g_conf.m_logTimingQuery || m_si->m_debug || g_conf.m_logDebugQuery) {
		int64_t now  = gettimeofdayInMilliseconds();
		int64_t took = now - m_startTime;
		logf(LOG_TIMING,"query: [%"PTRFMT"] Not found in cache. "
		     "Lookup took %"INT64" ms.",(PTRTYPE)this,took);
		m_startTime = now;
		logf(LOG_TIMING,"query: msg40: [%"PTRFMT"] Getting up to %"INT32" "
		     "(docToGet=%"INT32") docids", (PTRTYPE)this,
		     m_docsToGetVisible,  m_docsToGet);
	}

	// . if query has dirty words and family filter is on, set
	//   number of results to 0, and set the m_queryClen flag to true
	// . m_qbuf1 should be the advanced/composite query
	if ( m_si->m_familyFilter && 
	     getDirtyPoints ( m_si->m_sbuf1.getBufStart() , 
			      m_si->m_sbuf1.length() , 
			      0 ,
			      NULL ) ) {
		// make sure the m_numDocIds gets set to 0
		m_msg3a.reset();
		m_queryCensored = true;
		return true;
	}

	return getDocIds( false );
}

// . returns false if blocked, true otherwise
// . sets g_errno on error
bool Msg40::getDocIds ( bool recall ) {

	////
	//
	// NEW CODE FOR LAUNCHING one MSG3a per collnum to search a token
	//
	////
	m_num3aReplies = 0;
	m_num3aRequests = 0;

	// how many are we searching? usually just one.
	m_numCollsToSearch = m_si->m_collnumBuf.length() /sizeof(collnum_t);

	// make enough for ptrs
	int32_t need = sizeof(Msg3a *) * m_numCollsToSearch;
	if ( ! m_msg3aPtrBuf.reserve ( need ) ) return true;
	// cast the mem buffer
	m_msg3aPtrs = (Msg3a **)m_msg3aPtrBuf.getBufStart();

	// clear these out so we do not free them when destructing
	for ( int32_t i = 0 ; i < m_numCollsToSearch ;i++ )
		m_msg3aPtrs[i] = NULL;

	// use first guy in case only one coll we are searching, the std case
	if ( m_numCollsToSearch <= 1 )
		m_msg3aPtrs[0] = &m_msg3a;

	return federatedLoop();
}

bool Msg40::federatedLoop ( ) {

	// search the provided collnums (collections)
	collnum_t *cp = (collnum_t *)m_si->m_collnumBuf.getBufStart();

	// we modified m_rcache above to be true if we should read from cache
	int32_t maxAge = 0 ;
	if ( m_si->m_rcache ) maxAge = g_conf.m_indexdbMaxIndexListAge;


	// reset it
	Msg39Request mr;
	mr.reset();

	mr.m_maxAge                    = maxAge;
	mr.m_addToCache                = m_si->m_wcache;
	mr.m_docsToGet                 = m_docsToGet;
	mr.m_maxFacets                 = m_si->m_maxFacets;
	mr.m_niceness                  = m_si->m_niceness;
	mr.m_debug                     = m_si->m_debug          ;
	mr.m_getDocIdScoringInfo       = m_si->m_getDocIdScoringInfo;
	mr.m_doSiteClustering          = m_si->m_doSiteClustering    ;
	mr.m_hideAllClustered          = m_si->m_hideAllClustered;
	mr.m_familyFilter              = m_si->m_familyFilter;
	mr.m_doMaxScoreAlgo            = m_si->m_doMaxScoreAlgo;
	mr.m_doDupContentRemoval       = m_si->m_doDupContentRemoval ;
	mr.m_queryExpansion            = m_si->m_queryExpansion; 
	mr.m_boolFlag                  = m_si->m_boolFlag            ;
	mr.m_familyFilter              = m_si->m_familyFilter        ;
	mr.m_language                  = (unsigned char)m_si->m_queryLangId;
	mr.ptr_query                   = m_si->m_q.m_orig;
	mr.size_query                  = m_si->m_q.m_origLen+1;
	int32_t slen = 0; if ( m_si->m_sites ) slen=gbstrlen(m_si->m_sites)+1;
	mr.ptr_whiteList               = m_si->m_sites;
	mr.size_whiteList              = slen;
	mr.m_timeout                   = g_conf.m_msg40_msg39_timeout;
	mr.m_realMaxTop                = m_si->m_realMaxTop;

	mr.m_minSerpDocId              = m_si->m_minSerpDocId;
	mr.m_maxSerpScore              = m_si->m_maxSerpScore;
	mr.m_sameLangWeight            = m_si->m_sameLangWeight;

	//
	// how many docid splits should we do to avoid going OOM?
	//
	CollectionRec *cr = g_collectiondb.getRec(m_firstCollnum);
	RdbBase *base = NULL;
	if ( cr ) g_titledb.getRdb()->getBase(cr->m_collnum);
	int64_t numDocs = 0;
	if ( base ) numDocs = base->getNumTotalRecs();
	// for every 5M docids per host, lets split up the docid range
	// to avoid going OOM
	int32_t mult = numDocs / 5000000;
	if ( mult <= 0 ) mult = 1;

	int32_t nt = m_si->m_q.getNumTerms();
	int32_t numDocIdSplits = nt / 2; // ;/// 2;
	if ( numDocIdSplits <= 0 ) numDocIdSplits = 1;
	// and mult based on index size
	numDocIdSplits *= mult;
	// prevent going OOM for type:article AND html
	if ( numDocIdSplits < 5 ) numDocIdSplits = 5;
	log(LOG_DEBUG,"Msg40::federatedLoop: numDocIdSplits=%d", numDocIdSplits);
	//}

	if ( cr ) mr.m_maxQueryTerms = cr->m_maxQueryTerms; 
	else      mr.m_maxQueryTerms = 100;

	// special oom hack fix
	if ( cr && cr->m_isCustomCrawl && numDocIdSplits < 4 ) 
		numDocIdSplits = 4;

	// limit to 10
	if ( numDocIdSplits > 15 ) 
		numDocIdSplits = 15;
	// store it in the reuquest now
	mr.m_numDocIdSplits = numDocIdSplits;

	int32_t maxOutMsg3as = 1;

	// create new ones if searching more than 1 coll
	for ( int32_t i = m_num3aRequests ; i < m_numCollsToSearch ; i++ ) {

		// do not have more than this many outstanding
		if ( m_num3aRequests - m_num3aReplies >= maxOutMsg3as )
			// wait for it to return before launching another
			return false;

		// get it
		Msg3a *mp = m_msg3aPtrs[i];
		// stop if only searching one collection
		if ( ! mp ) {
			try { mp = new ( Msg3a); }
			catch ( ... ) {
				g_errno = ENOMEM;
				return true;
			}
			mnew(mp,sizeof(Msg3a),"tm3ap");
		}
		// error?
		if ( ! mp ) {
			log("msg40: Msg40::getDocIds() had error: %s",
			    mstrerror(g_errno));
			return true;
		}
		// assign it
		m_msg3aPtrs[i] = mp;
		// assign the request for it
		gbmemcpy ( &mp->m_rrr , &mr , sizeof(Msg39Request) );
		// then customize it to just search this collnum
		mp->m_rrr.m_collnum = cp[i];

		// launch a search request
		m_num3aRequests++;
		// this returns false if it would block and will call callback
		// m_si is actually contained in State0 in PageResults.cpp
		// and Msg40::m_si points to that. so State0's destructor
		// should call SearchInput's destructor which calls
		// Query's destructor to destroy &m_si->m_q here when done.
		if(!mp->getDocIds(&mp->m_rrr,&m_si->m_q,this,gotDocIdsWrapper))
			continue;
		if ( g_errno && ! m_errno ) 
			m_errno = g_errno;
		m_num3aReplies++;
	}

	// call again w/o parameters now
	return gotDocIds ( );
}	

// . uses parameters assigned to local member vars above
// . returns false if blocked, true otherwise
// . sets g_errno on error
void gotDocIdsWrapper ( void *state ) {
	Msg40 *THIS = (Msg40 *) state;
	// if this blocked, it returns false
	//if ( ! checkTurnOffRAT ( state ) ) return;
	THIS->m_num3aReplies++;
	// try to launch more if there are more colls left to search
	if ( THIS->m_num3aRequests < THIS->m_numCollsToSearch ) {
		THIS->federatedLoop ( );
		return;
	}
	// return if this blocked
	if ( ! THIS->gotDocIds() ) return;
	// now call callback, we're done
	THIS->m_callback ( THIS->m_state );
}

// . return false if blocked, true otherwise
// . sets g_errno on error
bool Msg40::gotDocIds ( ) {

	// return now if still waiting for a msg3a reply to get in
	if ( m_num3aReplies < m_num3aRequests ) return false;


	// if searching over multiple collections let's merge their docids
	// into m_msg3a now before we go forward
	// this will set g_errno on error, like oom
	if ( ! mergeDocIdsIntoBaseMsg3a() )
		log("msg40: error: %s",mstrerror(g_errno));


	// log the time it took for cache lookup
	int64_t now  = gettimeofdayInMilliseconds();

	if ( g_conf.m_logTimingQuery || m_si->m_debug||g_conf.m_logDebugQuery){
		int64_t took = now - m_startTime;
		logf(LOG_DEBUG,"query: msg40: [%"PTRFMT"] Got %"INT32" docids in %"INT64" ms",
		     (PTRTYPE)this,m_msg3a.getNumDocIds(),took);
		logf(LOG_DEBUG,"query: msg40: [%"PTRFMT"] Getting up to %"INT32" "
		     "summaries", (PTRTYPE)this,m_docsToGetVisible);
	}

	// save any covered up error
	if ( ! m_errno && m_msg3a.m_errno ) m_errno = m_msg3a.m_errno;
	//sanity check.  we might not have allocated due to out of memory
	if ( g_errno ) { m_errno = g_errno; return true; }

	// time this
	m_startTime = gettimeofdayInMilliseconds();

	// we haven't got any Msg20 responses as of yet or sent any requests
	m_numRequests  =  0;
	m_numReplies   =  0;

	// when returning search results in csv let's get the first 100
	// results and use those to determine the most common column headers
	// for the csv. any results past those that have new json fields we
	// will add a header for, but the column will not be labelled with
	// the header name unfortunately.
	m_needFirstReplies = 0;
	if ( m_si->m_format == FORMAT_CSV ) {
		m_needFirstReplies = m_msg3a.m_numDocIds;
		if ( m_needFirstReplies > 100 ) m_needFirstReplies = 100;
	}

	// . do not uncluster more than 5 docids! it slows things down.
	// . kind of a HACK until we do it right
	m_unclusterCount = 5;

	if ( ! m_urlTable.set ( m_msg3a.m_numDocIds * 2 ) ) {
		m_errno = g_errno;
		log("query: Failed to allocate memory for url deduping. "
		    "Not deduping search results.");
		return true;
	}

	// if only getting docids, skip summaries,topics, and references
	//	if ( m_si->m_docIdsOnly ) return launchMsg20s ( false );
	if ( m_si->m_docIdsOnly ) return true;

	// . alloc buf to hold all m_msg20[i] ptrs and the Msg20s they point to
	// . returns false and sets g_errno/m_errno on error
	// . salvage any Msg20s that we can if we are being re-called
	if ( ! reallocMsg20Buf() ) return true;

	// . launch a bunch of task that depend on the docids we got
	// . gigabits, reference pages and dmoz topics
	// . keep track of how many are out
	m_tasksRemaining = 0;

	// debug msg
	if ( m_si->m_debug || g_conf.m_logDebugQuery )
		logf(LOG_DEBUG,"query: [%"PTRFMT"] Getting topics/gigabits, "
		     "reference pages and dir pages.",(PTRTYPE)this);

	return launchMsg20s ( false );
}

bool Msg40::mergeDocIdsIntoBaseMsg3a() {

	// only do this if we were searching multiple collections, otherwise
	// all the docids are already in m_msg3a
	if ( m_numCollsToSearch <= 1 ) return true;
	
	// free any mem in use
	m_msg3a.reset();

	// count total docids into "td"
	int32_t td = 0LL;
	for ( int32_t i = 0 ; i < m_numCollsToSearch ; i++ ) {
		Msg3a *mp = m_msg3aPtrs[i];
		td += mp->m_numDocIds;
		// reset cursor for list of docids from this collection
		mp->m_cursor = 0;
		// add up here too
		m_msg3a.m_numTotalEstimatedHits += mp->m_numTotalEstimatedHits;
	}

	// setup to to merge all msg3as into our one m_msg3a
	int32_t need = 0;
	need += td * 8;
	need += td * sizeof(double);
	need += td * sizeof(key_t);
	need += td * 1;
	need += td * sizeof(collnum_t);
	// make room for the merged docids
	m_msg3a.m_finalBuf =  (char *)mmalloc ( need , "finalBuf" );
	m_msg3a.m_finalBufSize = need;
	// return true with g_errno set
	if ( ! m_msg3a.m_finalBuf ) return true;
	// parse the memory up into arrays
	char *p = m_msg3a.m_finalBuf;
	m_msg3a.m_docIds        = (int64_t *)p; p += td * 8;
	m_msg3a.m_scores        = (double    *)p; p += td * sizeof(double);
	m_msg3a.m_clusterRecs   = (key_t     *)p; p += td * sizeof(key_t);
	m_msg3a.m_clusterLevels = (char      *)p; p += td * 1;
	m_msg3a.m_scoreInfos    = NULL;
	m_msg3a.m_collnums      = (collnum_t *)p; p += td * sizeof(collnum_t);
	if ( p - m_msg3a.m_finalBuf != need ) { char *xx=NULL;*xx=0; }

	m_msg3a.m_numDocIds = td;

	//
	// begin the collection merge
	//

	int32_t next = 0;

 loop:

	// get next biggest score
	double max  = -1000000000.0;
	Msg3a *maxmp = NULL;

	for ( int32_t i = 0 ; i < m_numCollsToSearch ; i++ ) {
		// shortcut
		Msg3a *mp = m_msg3aPtrs[i];
		// get cursor
		int32_t cursor = mp->m_cursor;
		// skip if exhausted
		if ( cursor >= mp->m_numDocIds ) continue;
		// get his next score 
		double score = mp->m_scores[ cursor ];
		if ( score <= max ) continue;
		// got a new winner
		max = score;
		maxmp = mp;
	}

	// store him
	if ( maxmp ) {
		m_msg3a.m_docIds  [next] = maxmp->m_docIds[maxmp->m_cursor];
		m_msg3a.m_scores  [next] = maxmp->m_scores[maxmp->m_cursor];
		m_msg3a.m_collnums[next] = maxmp->m_rrr.m_collnum;
		m_msg3a.m_clusterLevels[next] = CR_OK;
		maxmp->m_cursor++;
		next++;
		goto loop;
	}

	// free tmp msg3as now
	for ( int32_t i = 0 ; i < m_numCollsToSearch ; i++ ) {
		if ( m_msg3aPtrs[i] == &m_msg3a ) continue;
		mdelete ( m_msg3aPtrs[i] , sizeof(Msg3a), "tmsg3a");
		delete  ( m_msg3aPtrs[i] );
		m_msg3aPtrs[i] = NULL;
	}

	return true;
}

// . returns false and sets g_errno/m_errno on error
// . makes m_msg3a.m_numDocIds ptrs to Msg20s. 
// . does not allocate a Msg20 in the buffer if the m_msg3a.m_clusterLevels[i]
//   is something other than CR_OK
bool Msg40::reallocMsg20Buf ( ) {

	// if the user only requested docids, we have no summaries
	if ( m_si->m_docIdsOnly ) return true;

	// . allocate m_buf2 to hold all our Msg20 pointers and Msg20 classes
	// . how much mem do we need?
	// . need space for the msg20 ptrs
	int64_t need = m_msg3a.m_numDocIds * sizeof(Msg20 *);
	// need space for the classes themselves, only if "visible" though
	for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++ ) 
		if ( m_msg3a.m_clusterLevels[i] == CR_OK ) 
			need += sizeof(Msg20);

	// MDW: try to preserve the old Msg20s if we are being re-called
	if ( m_buf2 ) {
		// we do not do recalls when streaming yet
		if ( m_si->m_streamResults ) { char *xx=NULL;*xx=0; }

		// make new buf
		char *newBuf = (char *)mmalloc(need,"Msg40d");
		// return false if it fails
		if ( ! newBuf ) { m_errno = g_errno; return false; }
		// fill it up
		char *p = newBuf;
		// point to our new array of Msg20 ptrs
		Msg20 **tmp = (Msg20 **)p;
		// skip over pointer array
		p += m_msg3a.m_numDocIds * sizeof(Msg20 *);
		// record start to set to m_msg20StartBuf
		char *pstart = p;
		// and count for m_numToFree
		int32_t pcount = 0;
		// fill in the actual Msg20s from the old buffer
		for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++ ) {
			// assume empty, because clustered, filtered, etc.
			tmp[i] = NULL;
			// if clustered, keep it as a NULL ptr
			if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
			// point it to its memory
			tmp[i] = (Msg20 *)p;
			// point to the next Msg20
			p += sizeof(Msg20);
			// init it
			tmp[i]->constructor();
			// count it
			pcount++;
			// skip it if it is a new docid, we do not have a Msg20
			// for it from the previous tier. IF it is from
			// the current tier, THEN it is new.
			//if ( m_msg3a.m_tiers[i] == m_msg3a.m_tier ) continue;
			// see if we can find this docid from the old list!
			int32_t k = 0;
			for ( ; k < m_numMsg20s ; k++ ) {
				// skip if NULL
				if ( ! m_msg20[k] ) continue;
				// if it never gave us a reply then skip it
				if ( ! m_msg20[k]->m_gotReply ) continue;
				//or if it had an error
				if ( m_msg20[k]->m_errno ) continue;
				// skip if no match
				if ( m_msg3a    .m_docIds[i] !=
				     m_msg20[k]->m_r->m_docId )//getDocId() )
					continue;
				// we got a match, grab its Msg20
				break;
			}
			// . skip if we could not match it... strange...
			// . no, because it may have been in the prev tier,
			//   from a split, but it was not in msg3a's final 
			//   merged list made in Msg3a::mergeLists(), but now 
			//   it is in there, with the previous tier, because
			//   we asked for more docids from msg3a.
			// . NO! why did we go to the next tier unnecessarily
			//   THEN? no again, because we did a msg3a recall
			//   and asked for more docids which required us
			//   going to the next tier, even though some (but
			//   not enough) docids remained in the previous tier.
			if ( k >= m_numMsg20s ) {
				/*
				logf(LOG_DEBUG,"query: msg40: could not match "
				     "docid %"INT64" (max=%"INT32") "
				     "to msg20. newBitScore=0x%hhx q=%s",
				     m_msg3a.m_docIds[i],
				     (char)m_msg3a.m_bitScores[i],
				     m_msg3a.m_q->m_orig);
				*/
				continue;
			}
			// it is from an older tier but never got the msg20 
			// for it? what happened? it got unclustered??
			if ( ! m_msg20[k] ) continue;

			// . otherwise copy the memory if available
			// . if m_msg20[i]->m_docId is set this will save us
			//   repeating a summary lookup
			tmp[i]->copyFrom ( m_msg20[k] );
		}
		// sanity check
		if ( p - (char *)tmp != need ) { char *xx = NULL; *xx = 0; }

		resetBuf2();

		// the new buf2 stuff
		m_numToFree     = pcount;
		m_msg20StartBuf = pstart;

		// re-assign the msg20 ptr to the ptrs
		m_msg20 = tmp;
		// update new count
		m_numMsg20s = m_msg3a.m_numDocIds;

		// assign to new mem
		m_buf2        = newBuf;
		m_bufMaxSize2 = need;

		// all done
		return true;
	}

	m_numMsg20s = m_msg3a.m_numDocIds;

	// when streaming because we can have hundreds of thousands of
	// search results we recycle a few msg20s to save mem
	if ( m_si->m_streamResults ) {
		int32_t max = MAX_OUTSTANDING_MSG20S * 2;
		if ( m_msg3a.m_numDocIds < max ) max = m_msg3a.m_numDocIds;
		need = 0;
		need += max * sizeof(Msg20 *);
		need += max * sizeof(Msg20);
		m_numMsg20s = max;
	}

	m_buf2        = NULL;
	m_bufMaxSize2 = need;

	// do the alloc
	if ( need ) m_buf2 = (char *)mmalloc ( need ,"Msg40msg20");
	if ( need && ! m_buf2 ) { m_errno = g_errno; return false; }
	// point to the mem
	char *p = m_buf2;
	// point to the array, then make p point to the Msg20 buffer space
	m_msg20 = (Msg20 **)p; 
	p += m_numMsg20s * sizeof(Msg20 *);
	// start free here
	m_msg20StartBuf = p;
	// set the m_msg20[] array to use this memory, m_buf20
	for ( int32_t i = 0 ; i < m_numMsg20s ; i++ ) {
		// assume empty
		m_msg20[i] = NULL;
		// if clustered, do a NULL ptr
		if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
		// point it to its memory
		m_msg20[i] = (Msg20 *)p;
		// call its constructor
		m_msg20[i]->constructor();
		// point to the next Msg20
		p += sizeof(Msg20);
		// remember num to free in reset() function
		m_numToFree++;
	}
	// remember how many we got in here in case we have to realloc above
	//m_numMsg20s = m_msg3a.m_numDocIds;

	return true;
}

void didTaskWrapper ( void* state ) {
	Msg40 *THIS = (Msg40 *) state;
	// one less task
	THIS->m_tasksRemaining--;
	// this returns false if blocked
	if ( ! THIS->launchMsg20s ( false ) ) return;
	// we are done, call the callback
	THIS->m_callback ( THIS->m_state );
}

bool Msg40::launchMsg20s ( bool recalled ) {

	// don't launch any more if client browser closed socket
	if ( m_socketHadError ) { char *xx=NULL; *xx=0; }

	// these are just like for passing to Msg39 above
	int32_t maxAge = 0 ;
	// may it somewhat jive with the search results caching, otherwise
	// it will tell me a search result was indexed like 3 days ago
	// when it was just indexed 10 minutes ago because the 
	// titledbMaxCacheAge was set way too high
	if ( m_si->m_rcache ) maxAge = g_conf.m_searchResultsMaxCacheAge;

	int32_t bigSampleRadius = 0;
	int32_t bigSampleMaxLen = 0;

	int32_t maxOut = (int32_t)MAX_OUTSTANDING_MSG20S;
	if ( g_udpServer.getNumUsedSlots() > 500 ) maxOut = 10;
	if ( g_udpServer.getNumUsedSlots() > 800 ) maxOut = 1;

	// if not deduping or site clustering, then
	// just skip over docids for speed.
	// don't bother with summaries we do not need
	if ( m_si && 
	     ! m_si->m_doDupContentRemoval &&
	     ! m_si->m_doSiteClustering &&
	     m_lastProcessedi == -1 ) {
		// start getting summaries with the result # they want
		m_lastProcessedi = m_si->m_firstResultNum-1;
		// assume we printed the summaries before
		m_printi = m_si->m_firstResultNum;
		m_numDisplayed = m_si->m_firstResultNum;
		// fake this so Msg40::gotSummary() can let us finish
		// because it checks m_numRequests <  m_msg3a.m_numDocIds
		m_numRequests = m_si->m_firstResultNum;
		m_numReplies  = m_si->m_firstResultNum;
		m_didSummarySkip = true;
		log("query: skipping summary generation of first %"INT32" docs",
		    m_si->m_firstResultNum);
	}

	// . launch a msg20 getSummary() for each docid
	// . m_numContiguous should preceed any gap, see below
	for ( int32_t i = m_lastProcessedi+1 ; i < m_msg3a.m_numDocIds ;i++ ) {
		// if the user only requested docids, do not get the summaries
		if ( m_si->m_docIdsOnly ) break;
		// hard limit
		if ( m_numRequests-m_numReplies >= maxOut ) break;
		// do not launch another until m_printi comes back because
		// all summaries are bottlenecked on printing him out now.
		if ( m_si->m_streamResults &&
		     // must have at least one outstanding summary guy
		     // otherwise we can return true below and cause
		     // the stream to truncate results in gotSummary()
		     //m_numReplies < m_numRequests &&
		     i >= m_printi + MAX_OUTSTANDING_MSG20S - 1 )
			break;

		// do not repeat for this i
		m_lastProcessedi = i;

		// start up a Msg20 to get the summary
		Msg20 *m = NULL;
		if ( m_si->m_streamResults ) {
			// there can be hundreds of thousands of results
			// when streaming, so recycle a few msg20s to save mem
			m = getAvailMsg20();
			// mark it so we know which docid it goes with
			m->m_ii = i;
		}
		else
			m = m_msg20[i];

		// if to a dead host, skip it
		int64_t docId = m_msg3a.m_docIds[i];
		uint32_t shardNum = g_hostdb.getShardNumFromDocId ( docId );
		// get the collection rec
		CollectionRec *cr = g_collectiondb.getRec(m_firstCollnum);
		// if shard is dead then do not send to it if not crawlbot
		if ( g_hostdb.isShardDead ( shardNum ) &&
		     cr &&
		     // diffbot urls.csv downloads often encounter dead
		     // hosts that are not really dead, so wait for it
		     ! cr->m_isCustomCrawl &&
		     // this is causing us to truncate streamed results
		     // too early when we have false positives that a 
		     // host is dead because the server is locking up 
		     // periodically
		     ! m_si->m_streamResults ) {
			log("msg40: skipping summary "
			    "lookup #%"INT32" of "
			    "docid %"INT64" for dead shard #%"INT32""
			    , i
			    , docId
			    , shardNum );
			m_numRequests++;
			m_numReplies++;
			continue;
		}


		// if msg20 ptr null that means the cluster level is not CR_OK
		if ( ! m ) {
			m_numRequests++;
			m_numReplies++;
			continue;
		}
		// . did we already TRY to get the summary for this docid?
		// . we might be re-called from the refilter: below
		// . if already did it, skip it
		// . Msg20::getSummary() sets m_docId, first thing
		if ( m_msg3a.m_docIds[i] == m->getRequestDocId() ) {
			m_numRequests++;
			m_numReplies++;
			continue;
		}

		// assume no error
		g_errno = 0;
		// debug msg
		if ( m_si->m_debug || g_conf.m_logDebugQuery )
			logf(LOG_DEBUG,"query: msg40: [%"PTRFMT"] Getting "
			     "summary #%"INT32" for docId=%"INT64"",
			     (PTRTYPE)this,i,m_msg3a.m_docIds[i]);
		// launch it
		m_numRequests++;
		// keep for-loops shorter with this
		//if ( i > m_maxiLaunched ) m_maxiLaunched = i;
		
		//getRec(m_si->m_coll2,m_si->m_collLen2);
		if ( ! cr ) {
			log("msg40: missing coll");
			g_errno = ENOCOLLREC;
			if ( m_numReplies < m_numRequests ) return false;
			return true;
		}


		// set the summary request then get it!
		Msg20Request req;
		Query *q = &m_si->m_q;
		req.ptr_qbuf             = q->getQuery();
		req.size_qbuf            = q->getQueryLen()+1;
		req.m_langId             = m_si->m_queryLangId;

		// set highlight query
		if ( m_si->m_highlightQuery &&
		     m_si->m_highlightQuery[0] ) {
			req.ptr_hqbuf = m_si->m_highlightQuery;
			req.size_hqbuf = gbstrlen(req.ptr_hqbuf)+1;
		}

		req.m_highlightQueryTerms = m_si->m_doQueryHighlighting;

		req.m_isDebug            = (bool)m_si->m_debug;

		if ( m_si->m_displayMetas && m_si->m_displayMetas[0] ) {
			int32_t dlen = gbstrlen(m_si->m_displayMetas);
			req.ptr_displayMetas     = m_si->m_displayMetas;
			req.size_displayMetas    = dlen+1;
		}

		req.m_docId              = m_msg3a.m_docIds[i];
		
		// if the msg3a was merged from other msg3as because we
		// were searching multiple collections...
		if ( m_msg3a.m_collnums )
			req.m_collnum = m_msg3a.m_collnums[i];
		// otherwise, just one collection
		else
			req.m_collnum = m_msg3a.m_rrr.m_collnum;

		req.m_numSummaryLines    = m_si->m_numLinesInSummary;
		req.m_maxCacheAge        = maxAge;
		req.m_wcache             = m_si->m_wcache; // addToCache
		req.m_state              = this;
		req.m_callback           = gotSummaryWrapper;
		req.m_niceness           = m_si->m_niceness;
		// 0 means not, 1 means is (should never be 2 at this point)
		req.m_boolFlag           = m_si->m_boolFlag;
		req.m_showBanned         = m_si->m_showBanned;
		req.m_includeCachedCopy  = m_si->m_includeCachedCopy;//bigsmpl
		req.m_getSectionVotingInfo   = m_si->m_getSectionVotingInfo;
		req.m_expected           = true;
		req.m_getSummaryVector   = true;
		req.m_bigSampleRadius    = bigSampleRadius;
		req.m_bigSampleMaxLen    = bigSampleMaxLen;
		//req.m_titleMaxLen        = 256;
		req.m_titleMaxLen = m_si->m_titleMaxLen; // cr->
		req.m_summaryMaxLen = cr->m_summaryMaxLen;

		// Line means excerpt 
		req.m_summaryMaxNumCharsPerLine = 
			m_si->m_summaryMaxNumCharsPerLine;

		// a special undocumented thing for getting <h1> tag
		req.m_getHeaderTag       = m_si->m_hr.getLong("geth1tag",0);
		// let "ns" parm override
		req.m_numSummaryLines    = m_si->m_numLinesInSummary;

		if(m_si->m_isMasterAdmin && m_si->m_format == FORMAT_HTML )
			req.m_getGigabitVector   = true;
		else    req.m_getGigabitVector   = false;
		if ( m_si->m_pqr_demFactCommonInlinks > 0.0 )
			req.m_getLinkInfo = true;
		// . buzz likes to do the &inlinks=1 parm to get inlinks
		// . use "&inlinks=1" for realtime inlink info, use 
		//   "&inlinks=2" to just get it from the title rec, which is 
		//   more stale, but does not take extra time or resources
		// . we "default" to the realtime stuff... i.e. since buzz
		//   is already using "&inlinks=1"
		if ( m_si->m_displayInlinks == 2 ) 
			req.m_getLinkInfo     = true;
		if ( m_si->m_displayOutlinks )
			req.m_getOutlinks     = true;

		if (m_si->m_queryMatchOffsets)
			req.m_getMatches = true;

		// it copies this using a serialize() function
		if ( ! m->getSummary ( &req ) ) continue;

		// got reply
		m_numReplies++;
		// . otherwise we got summary without blocking
		// . deal with an error
		if ( ! g_errno ) continue;
		// log it
		log("query: Had error getting summary: %s.",
		    mstrerror(g_errno));
		// record g_errno
		if ( ! m_errno ) m_errno = g_errno;
		// reset g_errno
		g_errno   = 0;
	}
	// return false if still waiting on replies
	if ( m_numReplies < m_numRequests ) return false;
	// do not re-call gotSummary() to avoid a possible recursive stack
	// explosion. this is only true if we are being called from 
	// gotSummary() already, so do not call it again!!
	if ( recalled ) 
		return true;
	// if we got nothing, that's it
	if ( m_msg3a.m_numDocIds <= 0 ) {
		// but if in streaming mode we still have to stream the
		// empty results back
		if ( m_si->m_streamResults ) return gotSummary ( );
		// otherwise, we're done
		return true;
	}
	// . i guess crash here for now
	// . seems like we can call reallocMsg20Buf() and the first 50
	//   can already be set, so we drop down to here... so don't core
	logf(LOG_DEBUG,"query: Had all msg20s already.");
	// . otherwise, we got everyone, so go right to the merge routine
	// . returns false if not all replies have been received 
	// . returns true if done
	// . sets g_errno on error
	return gotSummary ( );
}

Msg20 *Msg40::getAvailMsg20 ( ) {
	for ( int32_t i = 0 ; i < m_numMsg20s ; i++ ) {
		// m_inProgress is set to false right before it
		// calls Msg20::m_callback which is gotSummaryWrapper()
		// so we should be ok with this
		if ( m_msg20[i]->m_launched ) continue;
		return m_msg20[i];
	}
	// how can this happen???  THIS HAPPEND
	char *xx=NULL;*xx=0; 
	return NULL;
}

Msg20 *Msg40::getCompletedSummary ( int32_t ix ) {
	for ( int32_t i = 0 ; i < m_numMsg20s ; i++ ) {
		if ( m_msg20[i]->m_ii != ix ) continue;
		if ( m_msg20[i]->m_inProgress ) return NULL;
		return m_msg20[i];
	}
	return NULL;
}


bool gotSummaryWrapper ( void *state ) {
	Msg40 *THIS  = (Msg40 *)state;
	// inc it here
	THIS->m_numReplies++;
	// log every 1000 i guess
	if ( (THIS->m_numReplies % 1000) == 0 )
		log("msg40: got %"INT32" summaries out of %"INT32"",
		    THIS->m_numReplies,
		    THIS->m_msg3a.m_numDocIds);
	// it returns false if we're still awaiting replies
	if ( ! THIS->m_calledFacets && ! THIS->gotSummary ( ) ) return false;
	// lookup facets
	if ( THIS->m_si &&
	     ! THIS->m_si->m_streamResults &&
	     ! THIS->lookupFacets() ) 
		return false;
	// now call callback, we're done
	THIS->m_callback ( THIS->m_state );
	return true;
}

void doneSendingWrapper9 ( void *state , TcpSocket *sock ) {
	Msg40 *THIS = (Msg40 *)state;
	// the send completed, count it
	THIS->m_sendsIn++;
	// error?
	if ( THIS->m_sendsIn > THIS->m_sendsOut ) {
		log("msg40: sendsin > sendsout. bailing!!!");
		// try to prevent a core i haven't fixed right yet!!!
		// seems like a reply coming back after we've destroyed the
		// state!!!
		return;
	}
	// debug
	//g_errno = ETCPTIMEDOUT;
	// socket error? if client closes the socket midstream we get one.
	if ( g_errno ) {
		THIS->m_socketHadError = g_errno;
		log("msg40: streaming socket had error: %s",
		    mstrerror(g_errno));
		// i guess destroy the socket here so we don't get called again?

	}
	// clear it so we don't think it was a msg20 error below
	g_errno = 0;
	// try to send more... returns false if blocked on something
	if ( ! THIS->gotSummary() ) return;
	// all done!!!???
	THIS->m_callback ( THIS->m_state );
}

// . returns false if not all replies have been received (or timed/erroredout)
// . returns true if done (or an error finished us)
// . sets g_errno on error
bool Msg40::gotSummary ( ) {
	// now m_linkInfo[i] (for some i, i dunno which) is filled
	if ( m_si->m_debug || g_conf.m_logDebugQuery )
		logf(LOG_DEBUG,"query: msg40: [%"PTRFMT"] Got summary. "
		     "Total got=#%"INT32".",
		     (PTRTYPE)this,m_numReplies);

	// come back up here if we have to get more docids from Msg3a and
	// it gives us more right away without blocking, then we need to
	// re-filter them!
	// refilter:

	// did we have a problem getting this summary?
	if ( g_errno ) {
		// save it
		m_errno = g_errno;
		// log it
		if ( g_errno != EMISSINGQUERYTERMS )
			log("query: msg40: Got error getting summary: %s.",
			    mstrerror(g_errno));
		// reset g_errno
		g_errno = 0;
	}

	// initialize dedup table if we haven't already
	if ( ! m_dedupTable.isInitialized() &&
	     ! m_dedupTable.set (4,0,64,NULL,0,false,m_si->m_niceness,"srdt") )
		log("query: error initializing dedup table: %s",
		    mstrerror(g_errno));

	State0 *st = (State0 *)m_state;


 doAgain:

	SafeBuf *sb = &st->m_sb;

	sb->reset();

	// this is in PageResults.cpp
	if ( m_si && m_si->m_streamResults && ! m_printedHeader ) {
		// only print header once
		m_printedHeader = true;
		printHttpMime ( st );
		printSearchResultsHeader ( st );
	}

	for ( ; m_si && m_si->m_streamResults&&m_printi<m_msg3a.m_numDocIds ;
	      m_printi++){

		// if we are waiting on our previous send to complete... wait..
		if ( m_sendsOut > m_sendsIn ) break;

		// get summary for result #m_printi
		Msg20 *m20 = getCompletedSummary ( m_printi );

		// if printing csv we need the first 100 results back
		// to get the most popular csv headers for to print that
		// as the first row in the csv output. if we print a
		// results with a column not in the header row then we
		// augment the headers then and there, although the header
		// row will be blank for the new column, we can put
		// the new header row at the end of the file i guess. this way
		// we can immediately start streaming back the csv.
		if ( m_needFirstReplies ) {
			// need at least this many replies to process
			if ( m_numReplies < m_needFirstReplies )
				break;
			// ensure we got the TOP needFirstReplies in order
			// of their display to ensure consistency
			int32_t k;
			for ( k = 0 ; k < m_needFirstReplies ; k++ ) {
				Msg20 *xx = getCompletedSummary(k);
				if ( ! xx ) break;
				if ( ! xx->m_r && 
				     // and it did not have an error fetching
				     // because m_r could be NULL and m_errno
				     // is set to something like Bad Cached
				     // Document
				     ! xx->m_errno ) 
					break;
			}
			// if not all have come back yet, wait longer...
			if ( k < m_needFirstReplies ) break;
			// now make the csv header and print it
			printCSVHeaderRow ( sb );
			// and no longer need to do this logic
			m_needFirstReplies = 0;
		}

		// otherwise, get the summary for result #m_printi
		//Msg20 *m20 = m_msg20[m_printi];

		//if ( ! m20 ) {
		//	log("msg40: m20 NULL #%"INT32"",m_printi);
		//	continue;
		//}

		// if result summary #i not yet in, wait...
		if ( ! m20 ) 
			break;

		// wait if no reply for it yet
		//if ( m20->m_inProgress )
		//	break;

		if ( m20->m_errno ) {
			log("msg40: sum #%"INT32" error: %s",
			    m_printi,mstrerror(m20->m_errno));
			// make it available to be reused
			m20->reset();
			continue;
		}

		// get the next reply we are waiting on to print results order
		Msg20Reply *mr = m20->m_r;
		if ( ! mr ) break;
		//if ( ! mr ) { char *xx=NULL;*xx=0; }

		// primitive deduping. for diffbot json exclude url's from the
		// XmlDoc::m_contentHash32.. it will be zero if invalid i guess
		if ( m_si && m_si->m_doDupContentRemoval && // &dr=1
		     mr->m_contentHash32 &&
		     // do not dedup CT_STATUS results, those are
		     // spider reply "documents" that indicate the last
		     // time a doc was spidered and the error code or success
		     // code
		     mr->m_contentType != CT_STATUS &&
		     m_dedupTable.isInTable ( &mr->m_contentHash32 ) ) {
			//if ( g_conf.m_logDebugQuery )
			log("msg40: dup sum #%"INT32" (%"UINT32")"
			    "(d=%"INT64")",m_printi,
			    mr->m_contentHash32,mr->m_docId);
			// make it available to be reused
			m20->reset();
			continue;
		}

		// static int32_t s_bs = 0;
		// if ( (s_bs++ % 5) != 0 ) {
		// 	log("msg40: FAKE dup sum #%"INT32" (%"UINT32")(d=%"INT64")",m_printi,
		// 	    mr->m_contentHash32,mr->m_docId);
		// 	// make it available to be reused
		// 	m20->reset();
		// 	continue;
		// }


		// return true with g_errno set on error
		if ( m_si && m_si->m_doDupContentRemoval && // &dr=1
		     mr->m_contentHash32 &&
		     // do not dedup CT_STATUS results, those are
		     // spider reply "documents" that indicate the last
		     // time a doc was spidered and the error code or success
		     // code
		     mr->m_contentType != CT_STATUS &&
		     ! m_dedupTable.addKey ( &mr->m_contentHash32 ) ) {
			m_hadPrintError = true;
			log("msg40: error adding to dedup table: %s",
			    mstrerror(g_errno));
		}

		// assume we show this to the user
		m_numDisplayed++;
		//log("msg40: numdisplayed=%"INT32"",m_numDisplayed);

		// do not print it if before the &s=X start position though
		if ( m_si && m_numDisplayed <= m_si->m_firstResultNum ){
			if ( m_printCount == 0 ) 
				log("msg40: hiding #%"INT32" (%"UINT32")"
				    "(d=%"INT64")",
				    m_printi,mr->m_contentHash32,mr->m_docId);
		        m_printCount++;
			if ( m_printCount == 100 ) m_printCount = 0;
			m20->reset();
			continue;
		}

		// . ok, we got it, so print it and stream it
		// . this might set m_hadPrintError to true
		printSearchResult9 ( m_printi , &m_numPrintedSoFar , mr );

		//m_numPrintedSoFar++;
		//log("msg40: printedsofar=%"INT32"",m_numPrintedSoFar);

		// now free the reply to save memory since we could be 
		// streaming back 1M+. we call reset below, no need for this.
		//m20->freeReply();

		// return it so getAvailMsg20() can use it again
		// this will set m_launched to false
		m20->reset();
	}

	// . set it to true on all but the last thing we send!
	// . after each chunk of data we send out, TcpServer::sendChunk
	//   will call our callback, doneSendingWrapper9 
	if ( m_si->m_streamResults && st->m_socket )
		st->m_socket->m_streamingMode = true;


	// if streaming results, and too many results were clustered or
	// deduped then try to get more by merging the docid lists that
	// we already have from the shards. if this still does not provide
	// enough docids then we will need to issue a new msg39 request to
	// each shard to get even more docids from each shard.
	if ( m_si && m_si->m_streamResults &&
	     // this is coring as well on multi collection federated searches
	     // so disable that for now too. it is because Msg3a::m_r is
	     // NULL.
	     m_numCollsToSearch == 1 &&	     
	     // must have no streamed chunk sends out
	     m_sendsOut == m_sendsIn &&
	     // if we did not ask for enough docids and they were mostly
	     // dups so they got deduped, then ask for more.
	     // m_numDisplayed includes results before the &s=X parm.
	     // and so does m_docsToGetVisiable, so we can compare them.
	     m_numDisplayed < m_docsToGetVisible && 
	     // wait for us to have exhausted the docids we have merged
	     m_printi >= m_msg3a.m_numDocIds &&
	     // wait for us to have available msg20s to get summaries
	     m_numReplies == m_numRequests &&
	     // this is true if we can get more docids from merging
	     // more of the termlists from the shards together.
	     // otherwise, we will have to ask each shard for a
	     // higher number of docids.
	     m_msg3a.m_moreDocIdsAvail &&
	     // do not do this if client closed connection
	     ! m_socketHadError ) { //&&
		// doesn't work on multi-coll just yet, it cores.
		// MAKE it.
		//m_numCollsToSearch == 1 ) {
		// can it cover us?
		int32_t need = m_msg3a.m_docsToGet + 20;
		// note it
		log("msg40: too many summaries deduped. "
		    "getting more "
		    "docids from msg3a merge and getting summaries. "
		    "%"INT32" are visible, need %"INT32". "
		    "changing docsToGet from %"INT32" to %"INT32". "
		    "numReplies=%"INT32" numRequests=%"INT32"",
		    m_numDisplayed,
		    m_docsToGetVisible,
		    m_msg3a.m_docsToGet, 
		    need,
		    m_numReplies, 
		    m_numRequests);
		// merge more docids from the shards' termlists
		m_msg3a.m_docsToGet = need;
		// sanity. the original msg39request must be there
		if ( ! m_msg3a.m_r ) { char *xx=NULL;*xx=0; }
		// this should increase m_msg3a.m_numDocIds
		m_msg3a.mergeLists();
	}

	// if we've printed everything out and we are streaming, now
	// get the facet text. when done this should print the tail
	// like we do below. lookupFacets() should scan the facet values
	// and each value should have a docid with it that we do the lookup
	// on. and store the text into m_facetTextBuf safebuf, and make
	// the facet table have the offset of it in that safebuf.
	if ( m_si && 
	     m_si->m_streamResults && 
	     m_printi >= m_msg3a.m_numDocIds )
		if ( ! lookupFacets () ) return false;


	// . wrap it up with Next 10 etc.
	// . this is in PageResults.cpp
	if ( m_si && 
	     m_si->m_streamResults && 
	     ! m_printedTail &&
	     m_printi >= m_msg3a.m_numDocIds ) {
		m_printedTail = true;
		printSearchResultsTail ( st );
		if ( m_sendsIn < m_sendsOut ) { char *xx=NULL;*xx=0; }
		if ( g_conf.m_logDebugTcp )
			log("tcp: disabling streamingMode now");
		// this will be our final send
		if ( st->m_socket ) st->m_socket->m_streamingMode = false;
	}


	TcpServer *tcp = &g_httpServer.m_tcp;

	//g_conf.m_logDebugTcp = 1;

	// do we still own this socket? i am thinking it got closed somewhere
	// and the socket descriptor was re-assigned to another socket
	// getting a diffbot reply from XmLDoc::getDiffbotReply()
	if ( st->m_socket && 
	     st->m_socket->m_startTime != st->m_socketStartTimeHack ) {
		log("msg40: lost control of socket. sd=%i. the socket "
		    "descriptor closed on us and got re-used by someone else.",
		    (int)st->m_socket->m_sd);
		// if there wasn't already an error like 'broken pipe' then
		// set it here so we stop getting summaries if streaming.
		if ( ! m_socketHadError ) m_socketHadError = EBADENGINEER;
		// make it NULL to avoid us from doing anything to it
		// since sommeone else is using it now.
		st->m_socket = NULL;
		//g_errno = EBADENGINEER;
	}


	// . transmit the chunk in sb if non-zero length
	// . steals the allocated buffer from sb and stores in the 
	//   TcpSocket::m_sendBuf, which it frees when socket is
	//   ultimately destroyed or we call sendChunk() again.
	// . when TcpServer is done transmitting, it does not close the
	//   socket but rather calls doneSendingWrapper() which can call
	//   this function again to send another chunk
	// . when we are truly done sending all the data, then we set lastChunk
	//   to true and TcpServer.cpp will destroy m_socket when done.
	//   no, actually we just set m_streamingMode to false i guess above
	if ( sb->length() &&
	     // did client browser close the socket on us midstream?
	     ! m_socketHadError &&
	     st->m_socket &&
	     ! tcp->sendChunk ( st->m_socket , 
				sb  ,
				this ,
				doneSendingWrapper9 ) )
		// if it blocked, inc this count. we'll only call m_callback 
		// above when m_sendsIn equals m_sendsOut... and 
		// m_numReplies == m_numRequests
		m_sendsOut++;


	// writing on closed socket?
	if ( g_errno ) {
		if ( ! m_socketHadError ) m_socketHadError = g_errno;
		log("msg40: got tcp error : %s",mstrerror(g_errno));
		// disown it here so we do not damage in case it gets 
		// reopened by someone else
		st->m_socket = NULL;
	}

	// do we need to launch another batch of summary requests?
	if ( m_numRequests < m_msg3a.m_numDocIds && ! m_socketHadError ) {
		// . if we can launch another, do it
		// . say "true" here so it does not call us, gotSummary() and 
		//   do a recursive stack explosion
		// . this returns false if still waiting on more to come back
		if ( ! launchMsg20s ( true ) ) return false; 
		// it won't launch now if we are bottlnecked waiting for
		// m_printi's summary to come in
		if ( m_si->m_streamResults ) {
			// it won't launch any if we printed out enough as well
			// and it printed "waiting on remaining 0 to return".
			// we shouldn't be waiting for more to come in b/c
			// we are in gotSummart() so one just came in 
			// freeing up a msg20 to launch another, so assume
			// this means we are basically done. and it
			// set m_numRequests=m_msg3a.m_numDocIds etc.
			//if ( m_numRequests == m_msg3a.m_numDocIds )
			//	goto printTail;
			// otherwise, keep chugging
			goto complete;
		}
		// maybe some were cached?
		//goto refilter;
		// it returned true, so m_numRequests == m_numReplies and
		// we don't need to launch any more! but that does NOT
		// make sense because m_numContiguous < m_msg3a.m_numDocIds
		// . i guess the launch can fail because of oom... and
		//   end up returning true here... seen it happen, and
		//   we had full requests/replies for m_msg3a.m_numDocIds
		log("msg40: got all replies i guess");
		goto doAgain;
		//char *xx=NULL; *xx=0;
	}

 complete:

	// . ok, now i wait for all msg20s (getsummary) to come back in.
	// . TODO: evaluate if this hurts us
	if ( m_numReplies < m_numRequests )
		return false;

	// if streaming results, we are done
	if ( m_si && m_si->m_streamResults ) {
		// unless waiting for last transmit to complete
		if ( m_sendsOut > m_sendsIn ) return false;
		// delete everything! no, doneSendingWrapper9 does...
		//mdelete(st, sizeof(State0), "msg40st0");
		//delete st;
		// otherwise, all done!
		log("msg40: did not send last search result summary. "
		    "this=0x%"PTRFMT" because had error: %s",(PTRTYPE)this,
		    mstrerror(m_socketHadError));
		return true;
	}

	int64_t startTime = gettimeofdayInMilliseconds();
	int64_t took;

	// shortcut
	Query *q = &m_si->m_q;

	// loop over each clusterLevel and set it
	for ( int32_t i = 0 ; i < m_numReplies ; i++ ) {
		// did we skip the first X summaries because we were
		// not deduping/siteclustering/gettingGigabits?
		if ( m_didSummarySkip && i < m_si->m_firstResultNum )
			continue;
		// get current cluster level
		char *level = &m_msg3a.m_clusterLevels[i];
		// sanity check -- this is a transistional value msg3a should 
		// set it to something else!
		if ( *level == CR_GOT_REC         ) { char *xx=NULL; *xx=0; }
		if ( *level == CR_ERROR_CLUSTERDB ) { char *xx=NULL; *xx=0; }
		// skip if already "bad"
		if ( *level != CR_OK ) continue;
		// if the user only requested docids, we have no summaries
		if ( m_si->m_docIdsOnly ) break;
		// convenient var
		Msg20 *m = m_msg20[i];
		// get the Msg20 reply
		Msg20Reply *mr = m->m_r;
		// if no reply, all hosts must have been dead i guess so
		// filter out this guy
		if ( ! mr && ! m->m_errno ) {
			logf(LOG_DEBUG,"query: msg 20 reply was null.");
			m->m_errno = ENOHOSTS;
		}
		// if any msg20 has m_errno set, then set ours so at least the
		// xml feed will know there was a problem even though it may 
		// have gotten search results.
		// the BIG HACK is done in Msg20. Msg20::m_errno is set to 
		// something like EMISSINGQUERYTERMS if the document really
		// doesn't match the query, maybe because of indexdb corruption
		if ( m->m_errno ) {
			if ( m_si->m_debug || g_conf.m_logDebugQuery ) {
				logf( LOG_DEBUG, "query: result %" INT32 " (docid=%" INT64 ") had "
								 "an error (%s) and will not be shown.",
				      i, m_msg3a.m_docIds[i], mstrerror( m->m_errno ) );
			}

			// update our m_errno while here
			if ( ! m_errno ) {
				m_errno = m->m_errno;
			}

			if ( ! m_si->m_showErrors ) {
				*level = CR_ERROR_SUMMARY;
				continue;
			}
		}

		// a special case
		if ( mr && ( mr->m_errno == CR_RULESET_FILTERED || mr->m_errno == EDOCFILTERED ) ) {
			*level = CR_RULESET_FILTERED;
			continue;
		}

		if ( ! m_si->m_showBanned && mr && mr->m_isBanned ) {
			if ( m_si->m_debug || g_conf.m_logDebugQuery )
				logf( LOG_DEBUG, "query: result %" INT32 " (docid=%" INT64 ") is "
								 "banned and will not be shown.",
					  i, m_msg3a.m_docIds[i] );
			*level = CR_BANNED_URL;
			continue;
		}

		// corruption?
		if ( mr && !mr->ptr_ubuf ) {
			log( "msg40: got corrupt msg20 reply for docid %" INT64, mr->m_docId );
			*level = CR_BAD_URL;
			continue;
		}

		// filter out urls with <![CDATA in them
		if ( mr && strstr( mr->ptr_ubuf, "<![CDATA[" ) ) {
			*level = CR_BAD_URL;
			continue;
		}

		// also filter urls with ]]> in them
		if ( mr && strstr( mr->ptr_ubuf, "]]>" ) ) {
			*level = CR_BAD_URL;
			continue;
		}

		// filter empty title & summaries
		if ( mr && mr->size_tbuf <= 1 && mr->size_displaySum <= 1 ) {
			if ( ! m_si->m_showErrors ) {
				*level = CR_EMPTY_TITLE_SUMMARY;
				continue;
			}
		}
	}

	// . assume no dups removed
	// . we print "click here to show ommitted results" if this is true
	m_removedDupContent = false;

	// what is the deduping threshhold? 0 means do not do deuping
	int32_t dedupPercent = 0;
	if ( m_si->m_doDupContentRemoval && m_si->m_percentSimilarSummary )
		dedupPercent = m_si->m_percentSimilarSummary;
	// icc=1 turns this off too i think
	if ( m_si->m_includeCachedCopy ) dedupPercent = 0;
	// if the user only requested docids, we have no summaries
	if ( m_si->m_docIdsOnly ) dedupPercent = 0;

	// filter out duplicate/similar summaries
	for ( int32_t i = 0 ; dedupPercent && i < m_numReplies ; i++ ) {
		// skip if already invisible
		if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
		// Skip if invalid
		if ( m_msg20[i]->m_errno ) continue;

		// start with the first docid we have not yet checked!
		//int32_t m = oldNumContiguous;
		// get it
		Msg20Reply *mri = m_msg20[i]->m_r;
		// do not dedup CT_STATUS results, those are
		// spider reply "documents" that indicate the last
		// time a doc was spidered and the error code or 
		// success code
		if ( mri->m_contentType == CT_STATUS ) continue;
		// never let it be i
		//if ( m <= i ) m = i + 1;
		// see if any result lower-scoring than #i is a dup of #i
		for( int32_t m = i+1 ; m < m_numReplies ; m++ ) {
			// get current cluster level
			char *level = &m_msg3a.m_clusterLevels[m];
			// skip if already invisible
			if ( *level != CR_OK ) continue;
			// get it
			if ( m_msg20[m]->m_errno ) continue;

			Msg20Reply *mrm = m_msg20[m]->m_r;
			// do not dedup CT_STATUS results, those are
			// spider reply "documents" that indicate the last
			// time a doc was spidered and the error code or 
			// success code
			if ( mrm->m_contentType == CT_STATUS ) continue;
			// use gigabit vector to do topic clustering, etc.
			int32_t *vi = (int32_t *)mri->ptr_vbuf;
			int32_t *vm = (int32_t *)mrm->ptr_vbuf;
			//char  s  = g_clusterdb.
			//	getSampleSimilarity (vi,vm,VECTOR_REC_SIZE );
			float s ;
			s = computeSimilarity(vi,vm,NULL,NULL,NULL,
					      m_si->m_niceness);
			// skip if not similar
			if ( (int32_t)s < dedupPercent ) continue;
			// otherwise mark it as a summary dup
			if ( m_si->m_debug || g_conf.m_logDebugQuery )
				logf( LOG_DEBUG, "query: result #%"INT32" "
				      "(docid=%"INT64") is %.02f%% similar-"
				      "summary of #%"INT32" (docid=%"INT64")", 
				      m, m_msg3a.m_docIds[m] , 
				      s, i, m_msg3a.m_docIds[i] );
			*level = CR_DUP_SUMMARY;
                        //m_visibleContiguous--;
			m_removedDupContent = true;
			// uncluster the next clustered docid from this 
			// hostname below "m"
			if ( m_unclusterCount-- > 0 ) uncluster ( m );
		}
	}



        //
        // BEGIN URL NORMALIZE AND COMPARE
        // 
        
        // . ONLY DEDUP URL if it explicitly enabled AND we are not performing
        //   a site: or suburl: query.
        if(m_si->m_dedupURL && 
	   !q->m_hasPositiveSiteField && 
	   !q->m_hasSubUrlField) { 
		for(int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++) {
                        // skip if already invisible
                        if(m_msg3a.m_clusterLevels[i] != CR_OK) continue;
			// get it
			Msg20Reply *mr = m_msg20[i]->m_r;
                        // hash the URL all in lower case to catch wiki dups
			char *url  = mr-> ptr_ubuf;
			int32_t  ulen = mr->size_ubuf - 1;
			
			// since the redirect url is a more accurate 
			// representation of the conent do that if it exists.
			if ( mr->ptr_rubuf ) {
				url  = mr-> ptr_rubuf;
				ulen = mr->size_rubuf - 1;
			}

                        // fix for directories, sometimes they are indexed 
                        // without a trailing slash, so let's normalize to 
                        // this standard.
			if(url[ulen-1] == '/')
				ulen--;
			Url u;
                        u.set(url,ulen, false, false, false, false, false, 0x7fffffff);
                        url   = u.getHost();

                        if(u.getPathLen() > 1) {
                                // . remove sub-domain to fix conflicts with
                                //   sites having www,us,en,fr,de,uk,etc AND 
                                //   it redirects to the same page.
                                char *host = u.getHost();
                                char *mdom = u.getMidDomain();
                                if(mdom && host) {
                                        int32_t  hlen = mdom - host;
                                        if (isSubDom(host, hlen-1))
                                                url = mdom;
                                }
                        }

                        // adjust url string length
                        ulen -= url - u.getUrl();

			uint64_t h = hash64Lower_a(url, ulen);
                        int32_t slot = m_urlTable.getSlot(h);
                        // if there is no slot,this url doesn't exist => add it
                        if(slot == -1) {
                                m_urlTable.addKey(h,mr->m_docId);
                        }
                        else {
                                // If there was a slot, denote with the 
                                // cluster level URL already exited previously
                                char *level = &m_msg3a.m_clusterLevels[i];
                                if(m_si->m_debug || g_conf.m_logDebugQuery)
                                        logf(LOG_DEBUG, "query: result #%"INT32" "
                                                        "(docid=%"INT64") is the "
                                                        "same URL as "
                                                        "(docid=%"INT64")", 
                                                        i,m_msg3a.m_docIds[i], 
                                                        m_urlTable.
					     getValueFromSlot(slot));
                                *level = CR_DUP_URL;
                                //m_visibleContiguous--;
                                m_removedDupContent = true;
                        }
                }
        }

        //
        // END URL NORMALIZE AND COMPARE
        // 

	m_omitCount = 0;

	// count how many are visible!
	int32_t visible = 0;
	// loop over each clusterLevel and set it
	for ( int32_t i = 0 ; i < m_numReplies ; i++ ) {
		// get current cluster level
		char *level = &m_msg3a.m_clusterLevels[i];
		// on CR_OK
		if ( *level == CR_OK ) visible++;
		// otherwise count as ommitted
		else m_omitCount++;
	}

	// show time
	took = gettimeofdayInMilliseconds() - startTime;
	if ( took > 3 )
		log(LOG_INFO,"query: Took %"INT64" ms to do clustering and dup "
		    "removal.",took);

	// . let's wait for the tasks to complete before even trying to launch
	//   more than the first MAX_OUTSTANDING msg20s
	// . the msg3a re-call will end up re-doing our tasks as well! so we
	//   have to make sure they complete at this point
	if ( m_tasksRemaining > 0 ) return false;

	// debug
	bool debug = (m_si->m_debug || g_conf.m_logDebugQuery);
	for ( int32_t i = 0 ; debug && i < m_msg3a.m_numDocIds ; i++ ) {
		//uint32_t sh;
		//sh = g_titledb.getHostHash(*(key_t*)m_msg20[i]->m_vectorRec);
		int32_t cn = (int32_t)m_msg3a.m_clusterLevels[i];
		if ( cn < 0 || cn >= CR_END ) { char *xx=NULL;*xx=0; }
		char *s = g_crStrings[cn];
		if ( ! s ) { char *xx=NULL;*xx=0; }
		logf(LOG_DEBUG, "query: msg40 final hit #%"INT32") d=%"UINT64" "
		     "cl=%"INT32" (%s)", 
		     i,m_msg3a.m_docIds[i],(int32_t)m_msg3a.m_clusterLevels[i],s);
	}
	if ( debug )
		logf (LOG_DEBUG,"query: msg40: firstResult=%"INT32", "
		      "totalDocIds=%"INT32", resultsWanted=%"INT32" "
		      "visible=%"INT32" toGet=%"INT32" recallCnt=%"INT32"",
		      m_si->m_firstResultNum, m_msg3a.m_numDocIds ,
		      m_docsToGetVisible, visible,
		      //m_numContiguous, 
		      m_docsToGet , m_msg3aRecallCnt);

	// if we do not have enough visible, try to get more
	if ( visible < m_docsToGetVisible && m_msg3a.m_moreDocIdsAvail &&
	     // do not spin too long in this!
	     // TODO: fix this better somehow later
	     m_docsToGet <= 1000 &&
	     // doesn't work on multi-coll just yet, it cores
	     m_numCollsToSearch == 1 ) {
		// can it cover us?
		//int32_t need = m_msg3a.m_docsToGet + 20;
		int32_t need = m_docsToGet + 20;
		// increase by 25 percent as well
		need *= 1.25;
		// note it
		log("msg40: too many summaries invisible. getting more "
		    "docids from msg3a merge and getting summaries. "
		    "%"INT32" are visible, need %"INT32". "
		    "%"INT32" to %"INT32". "
		    "numReplies=%"INT32" numRequests=%"INT32"",
		    visible, m_docsToGetVisible,
		    m_msg3a.m_docsToGet, need,
		    m_numReplies, m_numRequests);

		// get more!
		m_docsToGet = need;
		// reset this before launch
		m_numReplies  = 0;
		m_numRequests = 0;
		// reprocess all!
		m_lastProcessedi = -1;
		// let's do it all from the top!
		return getDocIds ( true ) ;
	}

	// get time now
	int64_t now = gettimeofdayInMilliseconds();
	// . add the stat for how long to get all the summaries
	// . use purple for tie to get all summaries
	// . THIS INCLUDES Msg3a/Msg39 RECALLS!!!
	// . can we subtract that?
	g_stats.addStat_r ( 0           , 
			    m_startTime , 
			    now         ,
			    //"get_all_summaries",
			    0x008220ff  );
	// timestamp log
	if ( g_conf.m_logTimingQuery || m_si->m_debug )
		logf(LOG_DEBUG,"query: msg40: [%"PTRFMT"] Got %"INT32" summaries in "
		    "%"INT64" ms",
		     (PTRTYPE)this ,
		     visible, // m_visibleContiguous,
		     now - m_startTime );


	/////////////
	//
	//
	// prepare query term extra info for gigabits
	//
	////////////

	// english? TEST!
	unsigned char lang = m_si->m_queryLangId;
	// just print warning i guess
	if ( lang == 0 ) { 
		log("query: queryLang is 0 for q=%s",q->m_orig);
	}
	// we gotta use query TERMS not words, because the query may be
	// 'cd rom' and the phrase term will be 'cdrom' which is a good one
	// to use for gigabits! plus we got synonyms now!
	for ( int32_t i = 0 ; i < q->m_numTerms ; i++ ) {
		// shortcut
		QueryTerm *qt = &q->m_qterms[i];
		// assume ignored
		qt->m_popWeight = 0;
		qt->m_hash64d   = 0;
		// skip if ignored query stop word etc.
		if ( qt->m_ignored && qt->m_ignored != IGNORE_QUOTED )continue;
		// get the word or phrase
		char *s    = qt->m_term;
		int32_t  slen = qt->m_termLen;
		// use this special hash for looking up popularity in pop dict
		// i think it is just like hash64 but ignores spaces so we
		// can hash 'cd rom' as "cdrom". but i think we do this
		// now, so use m_termId as see...
		uint64_t qh = hash64d(s, slen);
		//int64_t qh = qt->m_termId;
		int32_t qpop;
		qpop = g_speller.getPhrasePopularity(s, qh, true,lang);
		int32_t qpopWeight;
		if       ( qpop < QPOP_ZONE_0 ) qpopWeight = QPOP_MULT_0;
		else if  ( qpop < QPOP_ZONE_1 ) qpopWeight = QPOP_MULT_1;
		else if  ( qpop < QPOP_ZONE_2 ) qpopWeight = QPOP_MULT_2;
		else if  ( qpop < QPOP_ZONE_3 ) qpopWeight = QPOP_MULT_3;
		else if  ( qpop < QPOP_ZONE_4 ) qpopWeight = QPOP_MULT_4;
		else                            qpopWeight = 1;
		// remember them in the query term
		qt->m_hash64d   = qh;
		qt->m_popWeight = qpopWeight;
	}

	// set m_moreToCome, if true, we print a "Next 10" link
	m_moreToCome = (visible >
			m_si->m_docsWanted+m_si->m_firstResultNum);
	if ( m_si->m_debug || g_conf.m_logDebugQuery ) 
		logf ( LOG_DEBUG, "query: msg40: more? %d",   m_moreToCome );

	// alloc m_buf, which should be NULL
	if ( m_buf ) { char *xx = NULL; *xx = 0; }

	// . we need to collapse m_msg3a.m_docIds[], etc. into m_docIds[] etc
	//   to be just the docids we wanted.
	// . at this point we should merge in all docids from all Msg40s from
	//   different clusters, etc.
	// . now alloc space for "docsWanted" m_docIds[], m_scores[], 
	//   m_bitScores[], m_clusterLevels[] and m_newMsg20[]

	//
	// HACK TIME
	//

	// . bury filtered/clustered docids from m_msg3a.m_docIds[]
	// . also remove result no in the request window specified by &s=X&n=Y
	//   where "s" is m_si->m_firstResultNum (which starts at 0) and "n" 
	//   is the number of results requested, m_si->m_docsWanted
	// . this is a bit of a hack (MDW)
	int32_t c = 0;
	int32_t v = 0;
	for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++ ) {
		// must ahve a cluster level of CR_OK (visible)
		// v is the visible count
		if ( ( m_msg3a.m_clusterLevels[i] != CR_OK ) || ( v++ < m_si->m_firstResultNum ) ) {
			// skip
			continue;
		}

		// we got a winner, save it
		m_msg3a.m_docIds        [c] = m_msg3a.m_docIds        [i];
		m_msg3a.m_scores        [c] = m_msg3a.m_scores        [i];
		m_msg3a.m_clusterLevels [c] = m_msg3a.m_clusterLevels [i];
		m_msg20                 [c] = m_msg20                 [i];

		if ( m_msg3a.m_scoreInfos ) {
			m_msg3a.m_scoreInfos [c] = m_msg3a.m_scoreInfos [i];
		}

		int32_t need = m_si->m_docsWanted;

		// if done, bail
		if ( ++c >= need ) {
			break;
		}
	}
	// reset the # of docids we got to how many we kept!
	m_msg3a.m_numDocIds = c;

	// debug
	for ( int32_t i = 0 ; debug && i < m_msg3a.m_numDocIds ; i++ )
		logf(LOG_DEBUG, "query: msg40 clipped hit #%"INT32") d=%"UINT64" "
		     "cl=%"INT32" (%s)", 
		     i,m_msg3a.m_docIds[i],(int32_t)m_msg3a.m_clusterLevels[i],
		     g_crStrings[(int32_t)m_msg3a.m_clusterLevels[i]]);

	//
	// END HACK
	// 

	// . uc = use cache?
	// . store in cache now if we need to
	bool uc = false;
	if ( m_si->m_useCache   ) uc = true;
	if ( m_si->m_wcache     ) uc = true;
	// . do not store if there was an error
	// . no, allow errors in cache since we often have lots of 
	//   docid not founds and what not, due to index corruption and
	//   being out of sync with titledb
	if ( m_errno            &&
	     // forgive "Record not found" errors, they are quite common
	     m_errno != ENOTFOUND &&
	     m_errno != EMISSINGQUERYTERMS ) {
		logf(LOG_DEBUG,"query: not storing in cache: %s",
		     mstrerror(m_errno));
		uc = false;
	}
	if ( m_si->m_docIdsOnly ) uc = false;



	// all done if not storing in cache
	if ( ! uc ) return true;
	// debug
	if ( m_si->m_debug )
		logf(LOG_DEBUG,"query: [%"PTRFMT"] Storing output in cache.",
		     (PTRTYPE)this);
	// store in this buffer
	char tmpBuf [ 64 * 1024 ];
	// use that
	char *p = tmpBuf;
	// how much room?
	int32_t tmpSize = getStoredSize();
	// unless too small
	if ( tmpSize > 64*1024 ) 
		p = (char *)mmalloc(tmpSize,"Msg40Cache");
	if ( ! p ) {
		// this is just for cachinig, not critical... ignore errors
		g_errno = 0;
		logf ( LOG_INFO ,
		       "query: Size of cached search results page (and "
		       "all associated data) is %"INT32" bytes. Max is %i. "
		       "Page not cached.", tmpSize, 32*1024 );
		return true;
	}
	// serialize into tmp
	int32_t nb = serialize ( p , tmpSize );
	// it must fit exactly
	if ( nb != tmpSize || nb == 0 ) {
		g_errno = EBADENGINEER;
		log (LOG_LOGIC,
		     "query: Size of cached search results page (%"INT32") "
		     "does not match what it should be. (%"INT32")",
		     nb, tmpSize );
		return true;
	}

	// free it, cache will copy it into its ring buffer
	if ( p != tmpBuf ) mfree ( p , tmpSize , "Msg40Cache" );
	// ignore errors
	g_errno = 0;
 	return true;
}

// m_msg3a.m_docIds[m] was filtered because it was a dup or something so we
// must "uncluster" the *next* docid from the same hostname that is clustered
void Msg40::uncluster ( int32_t m ) {
	// skip for now
	return;
}

int32_t Msg40::getStoredSize ( ) {
	// moreToCome=1
	int32_t size = 1;
	// msg3a
	size += m_msg3a.getStoredSize();
	// add each summary
	for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds; i++ ) {
		// getting rid of this makes it take up less room
		m_msg20[i]->clearLinks();
		m_msg20[i]->clearVectors();
		// if not visisble, do not store!
		if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
		// otherwise, store it
		size += m_msg20[i]->getStoredSize();
	}

	return size;
}

// . serialize ourselves for the cache
// . returns bytes written
// . returns -1 and sets g_errno on error
int32_t Msg40::serialize ( char *buf , int32_t bufLen ) {
	// set the ptr stuff
	char *p    = buf;
	char *pend = buf + bufLen;

	// miscellaneous
	*p++ = m_moreToCome;

	// msg3a:
	// m_numDocIds[]
	// m_docIds[]
	// m_scores[]
	// m_clusterLevels[]
	// m_totalHits (estimated)
	int32_t nb = m_msg3a.serialize ( p , pend );
	// return -1 on error
	if ( nb < 0 ) return -1;
	// otherwise, inc over it
	p += nb;

	// . then summary excerpts, keep them word aligned...
	// . TODO: make sure empty Msg20s are very little space!
	for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++ ) {
		// sanity check
		if ( m_msg3a.m_clusterLevels[i] == CR_OK && ! m_msg20[i] ) {
			char *xx = NULL; *xx = 0; }
		// if null skip it
		if ( ! m_msg20[i] ) continue;
		// do not store the big samples if we're not storing cached 
		// copy. if "includeCachedCopy" is true then the page itself 
		// will be the summary.
		//if ( ! m_si->m_includeCachedCopy )
		//	m_msg20[i]->clearBigSample();
		// getting rid of this makes it take up less room
		m_msg20[i]->clearLinks();
		m_msg20[i]->clearVectors();
		// if not visisble, do not store!
		if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
		// return -1 on error, g_errno should be set
		int32_t nb = m_msg20[i]->serialize ( p , pend - p ) ;
		// count it
		if ( m_msg3a.m_rrr.m_debug )
			log("query: msg40 serialize msg20size=%"INT32"",nb);

		if ( nb == -1 ) return -1;
		p += nb;
	}

	if ( m_msg3a.m_rrr.m_debug )
		log("query: msg40 serialize nd=%"INT32" "
		    "msg3asize=%"INT32" ",m_msg3a.m_numDocIds,nb);

	// return bytes stored
	return p - buf;
}

// . deserialize ourselves for the cache
// . returns bytes written
// . returns -1 and sets g_errno on error
int32_t Msg40::deserialize ( char *buf , int32_t bufSize ) {

	// we OWN the buffer
	m_buf        = buf;
	m_bufMaxSize = bufSize;

	// set the ptr stuff
	char *p    = buf;
	char *pend = buf + bufSize;

	// miscellaneous
	m_moreToCome      = *p++;

	// msg3a:
	// m_numDocIds
	// m_docIds[]
	// m_scores[]
	// m_clusterLevels[]
	// m_totalHits (estimated)
	int32_t nb = m_msg3a.deserialize ( p , pend );
	// return -1 on error
	if ( nb < 0 ) return -1;
	// otherwise, inc over it
	p += nb;

	// . alloc buf to hold all m_msg20[i] ptrs and the Msg20s they point to
	// . return -1 if this failed! it will set g_errno/m_errno already
	if ( ! reallocMsg20Buf() ) return -1;

	// MDW: then summary excerpts, keep them word aligned...
	for ( int32_t i = 0 ; i < m_msg3a.m_numDocIds ; i++ ) {
		// if flag is 0 that means a NULL msg20
		if ( m_msg3a.m_clusterLevels[i] != CR_OK ) continue;
		// return -1 on error, g_errno should be set
		int32_t x = m_msg20[i]->deserialize ( p , pend - p ) ;
		if ( x == -1 ) return -1;
		p += x;
	}

	// return bytes read
	return p - buf;
}


static char      *s_subDoms[] = {
        // Common Language sub-domains
        "en" ,
        "fr" ,
        "es" ,
        "ru" ,
        "zz" ,
        "ja" ,
        "tw" ,
        "cn" ,
        "ko" ,
        "de" ,
        "nl" ,
        "it" ,
        "fi" ,
        "sv" ,
        "no" ,
        "pt" ,
        "vi" ,
        "ar" ,
        "he" ,
        "id" ,
        "el" ,
        "th" ,
        "hi" ,
        "bn" ,
        "pl" ,
        "tl" ,
        // Common Country sub-domains
        "us" ,
        "uk" ,
        // Common web sub-domains
        "www" };
static HashTable  s_subDomTable;
static bool       s_subDomInitialized = false;
static bool initSubDomTable(HashTable *table, char *words[], int32_t size ){
	// set up the hash table
	if ( ! table->set ( size * 2 ) ) 
		return log(LOG_INIT,"build: Could not init sub-domain "
			   "table." );
	// now add in all the stop words
	int32_t n = (int32_t)size/ sizeof(char *); 
	for ( int32_t i = 0 ; i < n ; i++ ) {
		char      *sw    = words[i];
		int32_t       swlen = gbstrlen ( sw );
                int32_t h = hash32Lower_a(sw, swlen);
                int32_t slot = table->getSlot(h);
                // if there is no slot, this url doesn't exist => add it
                if(slot == -1)
                        table->addKey(h,0);
                else 
                        log(LOG_INIT,"build: Sub-domain table has duplicates");
	}
	return true;
}

bool isSubDom(char *s , int32_t len) {
	if ( ! s_subDomInitialized ) {
		s_subDomInitialized = 
			initSubDomTable(&s_subDomTable, s_subDoms, 
				      sizeof(s_subDoms));
		if (!s_subDomInitialized) return false;
	} 

	// get from table
        int32_t h = hash32Lower_a(s, len);
        if(s_subDomTable.getSlot(h) == -1)
                return false;
	return true;
}		

// . printSearchResult into "sb"
bool Msg40::printSearchResult9 ( int32_t ix , int32_t *numPrintedSoFar ,
				 Msg20Reply *mr ) {

	// . we stream results right onto the socket
	// . useful for thousands of results... and saving mem
	if ( ! m_si || ! m_si->m_streamResults ) { char *xx=NULL;*xx=0; }

	// get state0
	State0 *st = (State0 *)m_state;

	Msg40 *msg40 = &st->m_msg40;

	// then print each result
	// don't display more than docsWanted results
	if ( m_numPrinted >= msg40->getDocsWanted() ) {
		// i guess we can print "Next 10" link
		m_moreToCome = true;
		// hide if above limit
		if ( m_printCount == 0 )
			log(LOG_INFO,"msg40: hiding above docsWanted "
			    "#%"INT32" (%"UINT32")(d=%"INT64")",
			    m_printi,mr->m_contentHash32,mr->m_docId);
		m_printCount++;
		if ( m_printCount == 100 ) m_printCount = 0;
		// do not exceed what the user asked for
		return true;
	}

	// prints in xml or html
	if ( m_si->m_format == FORMAT_CSV ) {
		printJsonItemInCSV ( st , ix );
		//log("print: printing #%"INT32" csv",(int32_t)ix);
	}
	// print that out into st->m_sb safebuf
	else if ( ! printResult ( st , ix , numPrintedSoFar ) ) {
		// oom?
		if ( ! g_errno ) g_errno = EBADENGINEER;
		log("query: had error: %s",mstrerror(g_errno));
		m_hadPrintError = true;
	}

	// count it
	m_numPrinted++;

	return true;
}
	

bool printHttpMime ( State0 *st ) {

	SearchInput *si = &st->m_si;

	SafeBuf *sb = &st->m_sb;
	// reserve 1.5MB now!
	if ( ! sb->reserve(1500000 ,"pgresbuf" ) ) // 128000) )
		return true;
	// just in case it is empty, make it null terminated
	sb->nullTerm();

	char *ct = "text/csv";
	if ( si->m_format == FORMAT_JSON )
		ct = "application/json";
	if ( si->m_format == FORMAT_XML )
		ct = "text/xml";
	if ( si->m_format == FORMAT_HTML )
		ct = "text/html";
	//if ( si->m_format == FORMAT_TEXT )
	//	ct = "text/plain";
	if ( si->m_format == FORMAT_CSV )
		ct = "text/csv";

	// . if we haven't yet sent an http mime back to the user
	//   then do so here, the content-length will not be in there
	//   because we might have to call for more spiderdb data
	HttpMime mime;
	mime.makeMime ( -1, // totel content-lenght is unknown!
			0 , // do not cache (cacheTime)
			0 , // lastModified
			0 , // offset
			-1 , // bytesToSend
			NULL , // ext
			false, // POSTReply
			ct, // "text/csv", // contenttype
			"utf-8" , // charset
			-1 , // httpstatus
			NULL ); //cookie
	sb->safeMemcpy(mime.getMime(),mime.getMimeLen() );
	return true;
}

/////////////////
//
// CSV LOGIC from PageResults.cpp
//
/////////////////

#include "Json.h"

// 
// print header row in csv
//
bool Msg40::printCSVHeaderRow ( SafeBuf *sb ) {
	Msg20 *msg20s[100];
	int32_t i;
	for ( i = 0 ; i < m_needFirstReplies && i < 100 ; i++ ) {
		Msg20 *m20 = getCompletedSummary(i);
		if ( ! m20 ) break;
		msg20s[i] = m20;
	}

	int32_t numPtrs = 0;

	char tmp2[1024];
	SafeBuf nameBuf (tmp2, 1024);

	int32_t ct = 0;
	if ( msg20s[0] && msg20s[0]->m_r ) ct = msg20s[0]->m_r->m_contentType;

	CollectionRec *cr =g_collectiondb.getRec(m_firstCollnum);

	// . set up table to map field name to col for printing the json items
	// . call this from PageResults.cpp 
	printCSVHeaderRow2 ( sb , 
			     ct ,
			     cr ,
			     &nameBuf ,
			     &m_columnTable ,
			     msg20s ,
			     i , // numResults ,
			     &numPtrs 
			     );

	m_numCSVColumns = numPtrs;

	if ( ! sb->pushChar('\n') )
		return false;
	if ( ! sb->nullTerm() )
		return false;

	return true;
}

// returns false and sets g_errno on error
bool Msg40::printJsonItemInCSV ( State0 *st , int32_t ix ) {

	int32_t niceness = 0;

	//
	// get the json from the search result
	//
	Msg20 *m20 = getCompletedSummary(ix);
	if ( ! m20 ) return false;
	if ( m20->m_errno ) return false;
	if ( ! m20->m_r ) { char *xx=NULL;*xx=0; }
	Msg20Reply *mr = m20->m_r;
	// get content
	char *json = mr->ptr_content;
	// how can it be empty?
	if ( ! json ) { char *xx=NULL;*xx=0; }


	// parse the json
	Json jp;
	jp.parseJsonStringIntoJsonItems ( json , niceness );

	HashTableX *columnTable = &m_columnTable;
	int32_t numCSVColumns = m_numCSVColumns;

	//SearchInput *si = m_si;
	SafeBuf *sb = &st->m_sb;

	
	// make buffer space that we need
	char ttt[1024];
	SafeBuf ptrBuf(ttt,1024);
	int32_t maxCols = numCSVColumns;
	// allow for additionals colls
	maxCols += 100;
	int32_t need = maxCols * sizeof(JsonItem *);
	if ( ! ptrBuf.reserve ( need ) ) return false;
	JsonItem **ptrs = (JsonItem **)ptrBuf.getBufStart();

	// reset json item ptrs for csv columns. all to NULL
	memset ( ptrs , 0 , need );

	char tmp1[1024];
	SafeBuf tmpBuf (tmp1 , 1024);

	JsonItem *ji;

	///////
	//
	// print json item in csv
	//
	///////
	for ( ji = jp.getFirstItem(); ji ; ji = ji->m_next ) {

		// skip if not number or string
		if ( ji->m_type != JT_NUMBER && 
		     ji->m_type != JT_STRING )
			continue;

		// skip if not well suited for csv (see above comment)
		if ( ji->isInArray() ) continue;

		// . get the name of the item into "nameBuf"
		// . returns false with g_errno set on error
		if ( ! ji->getCompoundName ( tmpBuf ) )
			return false;

		// is it new?
		int64_t h64 = hash64n ( tmpBuf.getBufStart() );

		// ignore the "html" column
		if ( strcmp(tmpBuf.getBufStart(),"html") == 0 ) continue;

		int32_t slot = columnTable->getSlot ( &h64 ) ;
		// MUST be in there
		// get col #
		int32_t column = -1;
		if ( slot >= 0 )
			column =*(int32_t *)columnTable->getValueFromSlot ( slot);

		// sanity
		if ( column == -1 ) {
			// don't show it any more...
			continue;
		}

		// set ptr to it for printing when done parsing every field
		// for this json item
		ptrs[column] = ji;
	}

	// now print out what we got
	for ( int32_t i = 0 ; i < numCSVColumns ; i++ ) {
		// , delimeted
		if ( i > 0 ) sb->pushChar(',');
		// get it
		ji = ptrs[i];
		// skip if none
		if ( ! ji ) continue;

		// skip "html" field... too spammy for csv and > 32k causes
		// libreoffice calc to truncate it and break its parsing
		if ( ji->m_name && 
		     //! ji->m_parent &&
		     strcmp(ji->m_name,"html")==0)
			continue;

		//
		// get value and print otherwise
		//
		/*
		if ( ji->m_type == JT_NUMBER ) {
			// print numbers without double quotes
			if ( ji->m_valueDouble *10000000.0 == 
			     (double)ji->m_valueLong * 10000000.0 )
				sb->safePrintf("%"INT32"",ji->m_valueLong);
			else
				sb->safePrintf("%f",ji->m_valueDouble);
			continue;
		}
		*/

		int32_t vlen;
		char *str = ji->getValueAsString ( &vlen );

		// print the value
		sb->pushChar('\"');
		// get the json item to print out
		//int32_t  vlen = ji->getValueLen();
		// truncate
		char *truncStr = NULL;
		if ( vlen > 32000 ) {
			vlen = 32000;
			truncStr = " ... value truncated because "
				"Excel can not handle it. Download the "
				"JSON to get untruncated data.";
		}
		// print it out
		//sb->csvEncode ( ji->getValue() , vlen );
		sb->csvEncode ( str , vlen );
		// print truncate msg?
		if ( truncStr ) sb->safeStrcpy ( truncStr );
		// end the CSV
		sb->pushChar('\"');
	}

	sb->pushChar('\n');
	sb->nullTerm();

	return true;
}

// this is a safebuf of msg20s for doing facet string lookups
Msg20 *Msg40::getUnusedMsg20 ( ) {

	// make a safebuf of 50 of them if we haven't yet
	if ( m_unusedBuf.getCapacity() <= 0 ) {
		if ( ! m_unusedBuf.reserve ( (int32_t)MAX2 * sizeof(Msg20) ) ) {
			return NULL;
		}
		Msg20 *ma = (Msg20 *)m_unusedBuf.getBufStart();
		for ( int32_t i = 0 ; i < (int32_t)MAX2 ; i++ ) {
			ma[i].constructor();
			// if we don't update length then Msg40::resetBuf2() 
			// will fail to call Msg20::destructor on them
			m_unusedBuf.m_length += sizeof(Msg20);
		}
	}
		

	Msg20 *ma = (Msg20 *)m_unusedBuf.getBufStart();

	for ( int32_t i = 0 ; i < (int32_t)MAX2 ; i++ ) {
		// m_inProgress is set to false right before it
		// calls Msg20::m_callback which is gotSummaryWrapper()
		// so we should be ok with this
		if ( ma[i].m_inProgress ) continue;
		return &ma[i];
	}

	// how can this happen???
	char *xx=NULL;*xx=0; 
	return NULL;
}

static bool gotFacetTextWrapper ( void *state ) {
	Msg20 *m20 = (Msg20 *)state;
	Msg40 *THIS = (Msg40 *)m20->m_hack;
	THIS->gotFacetText(m20);
	return true;
}

void Msg40::gotFacetText ( Msg20 *msg20 ) {

	m_numMsg20sIn++;
	//log("msg40: numin=%"INT32"",m_numMsg20sIn);

	if ( ! msg20->m_r ) {
		log("msg40: msg20 reply is NULL");
		return;
	}

	char *buf = msg20->m_r->ptr_facetBuf;

	// null as well?
	if ( ! buf ) {
		log("msg40: ptr_facetBuf is NULL");
		// try to launch more msg20s
		lookupFacets();
		return;
	}

	char *p = buf;
	// skip query term string
	p += gbstrlen(p) + 1;
	// then <val32>,<str32>
	FacetValHash_t fvh = atoll(p);
	char *text = strstr ( p , "," );
	// skip comma. text could be truncated/ellipsis-sized
	if ( text ) text++;

	int32_t offset = m_facetTextBuf.length();
	m_facetTextBuf.safeStrcpy ( text );
	m_facetTextBuf.pushChar('\0');

	// initialize this if it needs it
	if ( m_facetTextTable.m_ks == 0 )
		m_facetTextTable.set(sizeof(FacetValHash_t),4,
				     64,NULL,0,false,0,"fctxtbl");

	// store in buffer
	m_facetTextTable.addKey ( &fvh , &offset );

	// try to launch more msg20s
	if ( ! lookupFacets() ) return;
}

// return false if blocked, true otherwise
bool Msg40::lookupFacets ( ) {

	if ( m_doneWithLookup ) return true;

	if ( !m_calledFacets ) {
		m_calledFacets = true;
		m_numMsg20sOut = 0;
		m_numMsg20sIn  = 0;
		m_j = 0;
		m_i = 0;
	}

	lookupFacets2();

	// if not done return false
	if ( m_numMsg20sOut > m_numMsg20sIn ) return false;

	m_doneWithLookup = true;

	// did nothing? return true so control resumes from where
	// lookupFacets() was called
	if ( m_numMsg20sOut == 0 ) return true;

	// hack: dec since gotSummaryWrapper incs this
	m_numReplies--;
	// . ok, we blocked, so call callback, etc.
	// . pretend we just got another summary
	gotSummaryWrapper ( this );

	return true;
}

void Msg40::lookupFacets2 ( ) {

	// scan each query term
	for ( ; m_i < m_si->m_q.getNumTerms() ; m_i++ ) {

		QueryTerm *qt = &m_si->m_q.m_qterms[m_i];
		// skip if not STRING facet. we don't need to lookup
		// numeric facets because we already have the # for compiling
		// and presenting on the search results page.
		if ( qt->m_fieldCode != FIELD_GBFACETSTR ) //&&
		     //qt->m_fieldCode != FIELD_GBFACETINT &&
		     //qt->m_fieldCode != FIELD_GBFACETFLOAT )
			continue;

		HashTableX *fht = &qt->m_facetHashTable;

		// now they are sorted in Msg3a.cpp
		int32_t *ptr = (int32_t *)qt->m_facetIndexBuf.getBufStart();
		int numPtrs = qt->m_facetIndexBuf.length()/sizeof(int32_t);

		// scan every value this facet has
		//for (  ; m_j < fht->getNumSlots() ; m_j++ ) {
		for (  ; m_j < numPtrs ; m_j++ ) {
			// skip empty slots
			//if ( ! fht->m_flags[m_j] ) continue;
			int32_t slot = ptr[m_j];
			// get hash of the facet value
			FacetValHash_t fvh ;
			fvh = *(int32_t *)fht->getKeyFromSlot(slot);
			//int32_t count = *(int32_t *)fht->getValFromSlot(j);
			// get the docid as well
			FacetEntry*fe=(FacetEntry *)fht->getValFromSlot(slot);
			// how many docids in the results had this valud?
			//int32_t      count = fe->m_count;
			// one of the docids that had it
			int64_t docId = fe->m_docId;

			// more than 50 already outstanding?
			if ( m_numMsg20sOut - m_numMsg20sIn >= MAX2 )
				// wait for some to come back
				return;

			// lookup docid that has this to get text
			Msg20 *msg20 = getUnusedMsg20();
			// wait if none available
			if ( ! msg20 ) return;

			// make the request
			Msg20Request req;
			req.m_docId = docId;
			// supply the query term so we know what to return.
			// it's either an xpath facet, a json/xml field facet
			// or a meta tag facet.
			SafeBuf tmp;
			tmp.safeMemcpy ( qt->m_term , qt->m_termLen );
			tmp.nullTerm();
			req. ptr_qbuf = tmp.getBufStart();
			req.size_qbuf = tmp.length() + 1; // include \0

			req.m_justGetFacets = true;
			// need to supply the hash of the facet value otherwise
			// if a doc has multiple values for a facet it always
			// returns the first one. so tell it we want this one.
			req.m_facetValHash  = fvh;

			msg20->m_hack = this;//(int32_t)this;

			req.m_state     = msg20;
			req.m_callback  = gotFacetTextWrapper;

			// TODO: fix this
			req.m_collnum = m_si->m_firstCollnum;

			// get it
			if ( ! msg20->getSummary ( &req ) ) {
				m_numMsg20sOut++;
				//log("msg40: numout=%"INT32"",m_numMsg20sOut);
				continue;
			}

			// must have been error otherwise
			log("facet: error getting text: %s",
			    mstrerror(g_errno));
		}
		// done! reset scan of inner loop
		m_j = 0;
	}
}

// this is new PageResults.cpp
bool replaceParm ( char *cgi , SafeBuf *newUrl , HttpRequest *hr ) ;

bool Msg40::printFacetTables ( SafeBuf *sb ) {

	char format = m_si->m_format;

	int32_t saved = sb->length();

        // If json, print beginning of json array
        if ( format == FORMAT_JSON ) {
                if ( m_si->m_streamResults ) {
                        // if we are streaming results in json, we may have hacked off
                        // the last ,\n so we need a comma to put it back
                        bool needComma = true;

                        // check if the last non-whitespace char in the
                        // buffer is a comma
                        for (int32_t i= sb->m_length-1; i >= 0; i--) {
                                char c = sb->getBufStart()[i];
                                if (c == '\n' || c == ' ') {
                                        // ignore whitespace chars
                                        continue;
                                }

                                // If the loop reaches this point, we have a
                                // non-whitespace char, so we break the loop
                                // either way
                                if (c == ',') {
                                        // last non-whitespace char is a comma,
                                        // so we don't need to add an extra one
                                        needComma = false;
                                }
                                break;
                        }

                        if ( needComma ) {
                                sb->safeStrcpy(",\n\n");
                        }
                }
                sb->safePrintf("\"facets\":[");
	}

        int numTablesPrinted = 0;
	for ( int32_t i = 0 ; i < m_si->m_q.getNumTerms() ; i++ ) {
		// only for html for now i guess
		//if ( m_si->m_format != FORMAT_HTML ) break;
		QueryTerm *qt = &m_si->m_q.m_qterms[i];
		// skip if not facet
		if ( qt->m_fieldCode != FIELD_GBFACETSTR &&
		     qt->m_fieldCode != FIELD_GBFACETINT &&
		     qt->m_fieldCode != FIELD_GBFACETFLOAT )
			continue;

		// if had facet ranges, print them out
		if ( printFacetsForTable ( sb , qt ) > 0 )
			numTablesPrinted++;
	}

        // If josn, print end of json array
        if ( format == FORMAT_JSON ) {
                if ( numTablesPrinted > 0 ) {
                        sb->m_length -= 2; // hack off trailing comma
			sb->safePrintf("],\n"); // close off json array
	        }
		// if no facets then do not print "facets":[]\n,
		else {
			// revert string buf to original length
			sb->m_length = saved;
			// and cap the string buf just in case
			sb->nullTerm();
		}
        }

	// if json, remove ending ,\n and make it just \n
	if ( format == FORMAT_JSON && sb->length() != saved ) {
		// remove ,\n
		sb->m_length -= 2;
		// make just \n
		sb->pushChar('\n');

		// search results will follow so put a comma here if not
		// streaming result. if we are streaming results we print
		// the facets after the results so we can take advantage
		// of the msg20 summary lookups we already did to get the
		// facet text.
		if ( ! m_si->m_streamResults ) 
			sb->safePrintf(",\n");
	}

	return true;
}

int32_t Msg40::printFacetsForTable ( SafeBuf *sb , QueryTerm *qt ) {
	HashTableX *fht = &qt->m_facetHashTable;
	
	int32_t *ptrs = (int32_t *)qt->m_facetIndexBuf.getBufStart();
	int32_t numPtrs = qt->m_facetIndexBuf.length() / sizeof(int32_t);

	if ( numPtrs == 0 )
		return 0;

	int32_t numPrinted = 0;

	// now scan the slots and print out
	HttpRequest *hr = &m_si->m_hr;

	bool isString = false;
	bool isFloat  = false;
	bool isInt = false;
	if ( qt->m_fieldCode == FIELD_GBFACETSTR ) isString = true;
	if ( qt->m_fieldCode == FIELD_GBFACETFLOAT ) isFloat = true;
	if ( qt->m_fieldCode == FIELD_GBFACETINT   ) isInt = true;
	char format = m_si->m_format;
	// a new table for each facet query term
	bool needTable = true;

	// print out the dumps
	for ( int32_t x= 0 ; x < numPtrs ; x++ ) {
		// skip empty slots
		//if ( ! fht->m_flags[j] ) continue;
		int32_t j = ptrs[x];
		// this was originally 32 bit hash of the facet val
		// but now it is 64 bit i guess
		FacetValHash_t *fvh ;
		fvh = (FacetValHash_t *)fht->getKeyFromSlot(j);
		FacetEntry *fe;
		fe = (FacetEntry *)fht->getValueFromSlot(j);
		int32_t count = 0;
		int64_t allCount = 0;
		// could be empty if range had no values in it
		if ( fe ) {
			count = fe->m_count;
			allCount = fe->m_outsideSearchResultsCount;
		}

		char *text = NULL;

		char *termPtr = qt->m_term;
		int32_t  termLen = qt->m_termLen;
		if ( termPtr[0] == ' ' ) { termPtr++; termLen--; }
		if ( strncasecmp(termPtr,"gbfacetstr:",11)== 0 ) {
			termPtr += 11; termLen -= 11; }
		if ( strncasecmp(termPtr,"gbfacetint:",11)== 0 ) {
			termPtr += 11; termLen -= 11; }
		if ( strncasecmp(termPtr,"gbfacetfloat:",13)== 0 ) {
			termPtr += 13; termLen -= 13; }
		char tmpBuf[64];
		SafeBuf termBuf(tmpBuf,64);
		termBuf.safeMemcpy(termPtr,termLen);
		termBuf.nullTerm();
		char *term = termBuf.getBufStart();

		char tmp9[128];
		SafeBuf sb9(tmp9,128);

		QueryWord *qw= qt->m_qword;

			
		if ( qt->m_fieldCode == FIELD_GBFACETINT && 
		     qw->m_numFacetRanges == 0 ) {
			sb9.safePrintf("%"INT32"",(int32_t)*fvh);
			text = sb9.getBufStart();
		}

		if ( qt->m_fieldCode == FIELD_GBFACETFLOAT 
		     && qw->m_numFacetRanges == 0 ) {
			sb9.printFloatPretty ( *(float *)fvh );
			text = sb9.getBufStart();
		}

		int32_t k2 = -1;

		// get the facet range that this FacetEntry represents (int)
		for ( int32_t k = 0 ; k < qw->m_numFacetRanges; k++ ) {
			if ( qt->m_fieldCode != FIELD_GBFACETINT )
				break;
			if ( *(int32_t *)fvh < qw->m_facetRangeIntA[k])
				continue;
			if ( *(int32_t *)fvh >= qw->m_facetRangeIntB[k])
				continue;
			sb9.safePrintf("[%"INT32"-%"INT32")"
				       ,qw->m_facetRangeIntA[k]
				       ,qw->m_facetRangeIntB[k]
				       );
			text = sb9.getBufStart();
			k2 = k;
		}

		// get the facet range that this FacetEntry represents (float)
		for ( int32_t k = 0 ; k < qw->m_numFacetRanges; k++ ) {
			if ( qt->m_fieldCode != FIELD_GBFACETFLOAT )
				break;
			if ( *(float *)fvh < qw->m_facetRangeFloatA[k])
				continue;
			if ( *(float *)fvh >= qw->m_facetRangeFloatB[k])
				continue;
			sb9.pushChar('[');
			sb9.printFloatPretty(qw->m_facetRangeFloatA[k]);
			sb9.pushChar('-');
			sb9.printFloatPretty(qw->m_facetRangeFloatB[k]);
			sb9.pushChar(')');
			sb9.nullTerm();
			text = sb9.getBufStart();
			k2 = k;
		}


		// lookup the text representation, whose hash is *fvh
		if ( qt->m_fieldCode == FIELD_GBFACETSTR ) {
			int32_t *offset;
			offset =(int32_t *)m_facetTextTable.getValue(fvh);
			// wtf?
			if ( ! offset ) {
				log("msg40: missing facet text for "
				    "val32=%"UINT32"",
				    (uint32_t)*fvh);
				continue;
			}
			text = m_facetTextBuf.getBufStart() + *offset;
		}


		if ( format == FORMAT_XML ) {
			numPrinted++;
			sb->safePrintf("\t<facet>\n"
				       "\t\t<field>%s</field>\n"
				       , term );
			sb->safePrintf("\t\t<totalDocsWithField>%"INT64""
				       "</totalDocsWithField>\n"
				       , qt->m_numDocsThatHaveFacet );
			sb->safePrintf("\t\t<totalDocsWithFieldAndValue>"
				       "%"INT64""
				       "</totalDocsWithFieldAndValue>\n"
				       , allCount );
			sb->safePrintf("\t\t<value>");

			if ( isString )
				sb->safePrintf("<![CDATA[%"UINT32",",
					       (uint32_t)*fvh);
			sb->cdataEncode ( text );
			if ( isString )
				sb->safePrintf("]]>");
			sb->safePrintf("</value>\n");
			sb->safePrintf("\t\t<docCount>%"INT32""
				       "</docCount>\n"
				       ,count);
			// some stats now for floats
			if ( isFloat && fe->m_count ) {
				sb->safePrintf("\t\t<average>");
				double sum = *(double *)&fe->m_sum;
				double avg = sum/(double)fe->m_count;
				sb->printFloatPretty ( (float)avg );
				sb->safePrintf("\t\t</average>\n");
				sb->safePrintf("\t\t<min>");
				float min = *(float *)&fe->m_min;
				sb->printFloatPretty ( min );
				sb->safePrintf("</min>\n");
				sb->safePrintf("\t\t<max>");
				float max = *(float *)&fe->m_max;
				sb->printFloatPretty ( max );
				sb->safePrintf("</max>\n");
			}
			// some stats now for ints
			if ( isInt && fe->m_count ) {
				sb->safePrintf("\t\t<average>");
				int64_t sum = fe->m_sum;
				double avg = (double)sum/(double)fe->m_count;
				sb->printFloatPretty ( (float)avg );
				sb->safePrintf("\t\t</average>\n");
				sb->safePrintf("\t\t<min>");
				int32_t min = fe->m_min;
				sb->safePrintf("%"INT32"</min>\n",min);
				sb->safePrintf("\t\t<max>");
				int32_t max = fe->m_max;
				sb->safePrintf("%"INT32"</max>\n",max);
			}
			sb->safePrintf("\t</facet>\n");
			continue;
		}

		// print that out
		if ( needTable && format == FORMAT_HTML ) {
			needTable = false;

			sb->safePrintf("<div id=facets "
				       "style="
				       "padding:5px;"
				       "position:relative;"
				       "border-width:3px;"
				       "border-right-width:0px;"
				       "border-style:solid;"
				       "margin-left:10px;"
				       "border-top-left-radius:10px;"
				       "border-bottom-left-radius:10px;"
				       "border-color:blue;"
				       "background-color:white;"
				       "border-right-color:white;"
				       "margin-right:-3px;"
				       ">"

				       "<table cellspacing=7>"
				       "<tr><td width=200px; "
				       "valign=top>"
				       "<center>"
				       "<img src=/facets40.jpg>"
				       "</center>"
				       "<br>"
				       );
			sb->safePrintf("<font color=gray>"
				       "values for</font> "
				       "<b>%s</b></td></tr>\n",
				       term);
		}


		if ( format == FORMAT_JSON ) {
			numPrinted++;
			sb->safePrintf("{\n"
				       "\t\"field\":\"%s\",\n"
				       , term 
				       );
			sb->safePrintf("\t\"totalDocsWithField\":%"INT64""
				       ",\n", qt->m_numDocsThatHaveFacet );
			sb->safePrintf("\t\"totalDocsWithFieldAndValue\":"
				       "%"INT64""
				       ",\n", 
				       allCount );
			sb->safePrintf("\t\"value\":\"");

			if (  isString )
				sb->safePrintf("%"UINT32","
					       , (uint32_t)*fvh);
			sb->jsonEncode ( text );
			//if ( isString )
			// just use quotes for ranges like "[1-3)" now
			sb->safePrintf("\"");
			sb->safePrintf(",\n");

			sb->safePrintf("\t\"docCount\":%"INT32""
				       , count );
			// if it's a # then we print stats after
			if ( isString || fe->m_count == 0 )
				sb->safePrintf("\n");
			else
				sb->safePrintf(",\n");
				

			// some stats now for floats
			if ( isFloat && fe->m_count ) {
				sb->safePrintf("\t\"average\":");
				double sum = *(double *)&fe->m_sum;
				double avg = sum/(double)fe->m_count;
				sb->printFloatPretty ( (float)avg );
				sb->safePrintf(",\n");
				sb->safePrintf("\t\"min\":");
				float min = *(float *)&fe->m_min;
				sb->printFloatPretty ( min );
				sb->safePrintf(",\n");
				sb->safePrintf("\t\"max\":");
				float max = *(float *)&fe->m_max;
				sb->printFloatPretty ( max );
				sb->safePrintf("\n");
			}
			// some stats now for ints
			if ( isInt && fe->m_count ) {
				sb->safePrintf("\t\"average\":");
				int64_t sum = fe->m_sum;
				double avg = (double)sum/(double)fe->m_count;
				sb->printFloatPretty ( (float)avg );
				sb->safePrintf(",\n");
				sb->safePrintf("\t\"min\":");
				int32_t min = fe->m_min;
				sb->safePrintf("%"INT32",\n",min);
				sb->safePrintf("\t\"max\":");
				int32_t max = fe->m_max;
				sb->safePrintf("%"INT32"\n",max);
			}

			sb->safePrintf("}\n,\n" );

			continue;
		}


		// make the cgi parm to add to the original url
		char nsbuf[128];
		SafeBuf newStuff(nsbuf,128);
		// they are all ints...
		//char *suffix = "int";
		//if ( qt->m_fieldCode == FIELD_GBFACETFLOAT )
		//	suffix = "float";
		//newStuff.safePrintf("prepend=gbequalint%%3A");
		if ( qt->m_fieldCode == FIELD_GBFACETINT &&
		     qw->m_numFacetRanges > 0 ) {
		     int32_t min = qw->m_facetRangeIntA[k2];
		     int32_t max = qw->m_facetRangeIntB[k2];
		     if ( min == max )
			     newStuff.safePrintf("prepend="
						 "gbequalint%%3A%s%%3A%"UINT32"+"
						 ,term
						 ,(int32_t)*fvh);
		     else
			     newStuff.safePrintf("prepend="
						 "gbminint%%3A%s%%3A%"UINT32"+"
						 "gbmaxint%%3A%s%%3A%"UINT32"+"
						 ,term
						 ,min
						 ,term
						 ,max-1
						 );
		}
		else if ( qt->m_fieldCode == FIELD_GBFACETFLOAT &&
			  qw->m_numFacetRanges > 0 ) {
			float min = qw->m_facetRangeFloatA[k2];
			float max = qw->m_facetRangeFloatB[k2];
			if ( min == max )
				newStuff.safePrintf("prepend="
						    "gbequalfloat%%3A%s%%3A%f+"
						    ,term
						    ,*(float *)fvh);
			else
			newStuff.safePrintf("prepend="
					    "gbminfloat%%3A%s%%3A%f+"
					    "gbmaxfloat%%3A%s%%3A%f+"
					    ,term
					    ,min
					    ,term
					    ,max
					    );
		}
		else if ( qt->m_fieldCode == FIELD_GBFACETFLOAT )
			newStuff.safePrintf("prepend="
					    "gbequalfloat%%3A%s%%3A%f",
					    term,
					    *(float *)fvh);
		else if ( qt->m_fieldCode == FIELD_GBFACETINT )
			newStuff.safePrintf("prepend="
					    "gbequalint%%3A%s%%3A%"UINT32"",
					    term,
					    (int32_t)*fvh);
		else if ( qt->m_fieldCode == FIELD_GBFACETSTR &&
			  // in XmlDoc.cpp the gbxpathsitehash123456: terms
			  // call hashFacets2() separately with val32 
			  // equal to the section inner hash which is not
			  // an exact hash of the string using hash32()
			  // unfortunately, so we can't use gbfieldmatch:
			  // which is case sensitive etc.
			  !strncmp(qt->m_term,
				   "gbfacetstr:gbxpathsitehash",26) )
			newStuff.safePrintf("prepend="
					    "gbequalint%%3Agbfacetstr%%3A"
					    "%s%%3A%"UINT32"",
					    term,
					    (int32_t)*fvh);
		else if ( qt->m_fieldCode == FIELD_GBFACETSTR ) {
			newStuff.safePrintf("prepend="
					    "gbfieldmatch%%3A%s%%3A%%22"
					    ,term
					    //"gbequalint%%3A%s%%3A%"UINT32""
					    //,(int32_t)*fvh
					    );
			newStuff.urlEncode(text);
			newStuff.safePrintf("%%22");
		}

		// get the original url and add 
		// &prepend=gbequalint:gbhopcount:1 type stuff to it
		SafeBuf newUrl;
		replaceParm ( newStuff.getBufStart(), &newUrl , hr );

		numPrinted++;

		// print the facet in its numeric form
		// we will have to lookup based on its docid
		// and get it from the cached page later
		sb->safePrintf("<tr><td width=200px; valign=top>"
			       //"<a href=?search="//gbfacet%3A"
			       //"%s:%"UINT32""
			       // make a search to just show those
			       // docs from this facet with that
			       // value. actually gbmin/max would work
			       "<a href=\"%s\">"
			       , newUrl.getBufStart()
			       );

		sb->safePrintf("%s (%"UINT32" documents)"
			       "</a>"
			       "</td></tr>\n"
			       ,text
			       ,count); // count for printing
	}

	if ( ! needTable && format == FORMAT_HTML ) 
		sb->safePrintf("</table></div><br>\n");

	return numPrinted;
}
