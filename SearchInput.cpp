#include "gb-include.h"

#include "SearchInput.h"
#include "Parms.h"         // g_parms
#include "Pages.h"         // g_msg
#include "LanguageIdentifier.h"
#include "CountryCode.h"
#include "PageResults.h"

#include "third-party/cld2/public/compact_lang_det.h"
#include "third-party/cld2/public/encodings.h"

SearchInput::SearchInput() {
	reset();
}
SearchInput::~SearchInput() {
	reset();
}
void SearchInput::reset ( ) {
}

void SearchInput::clear ( int32_t niceness ) {
	// reset it first
	reset();
	// set all to 0 just to avoid any inconsistencies
	int32_t size = (char *)&m_END_TEST - (char *)&m_START;
	memset ( &m_START , 0x00 , size );
	m_sbuf1.reset();
	m_sbuf2.reset();

	// set these
	m_numLinesInSummary  = 2;
	m_docsWanted         = 10;
	m_boolFlag           = 2;
	m_maxQueryTerms      = 1000;
	m_niceness           = niceness;

	//m_defaultSortLanguageLen = 0;
}


// . make a key for caching the search results page based on this input
// . do not use all vars, like the m_*ToDisplay should not be included
key_t SearchInput::makeKey ( ) {
	// hash the query
	int32_t       n       = m_q.getNumTerms  ();
	key_t k;
	k.n1 = 0;

	// user defined weights, for weighting each query term separately
	for ( int32_t i = 0 ; i < n ; i++ ) {
		k.n0 = hash64 ((char *)&m_q.m_qterms[i].m_termId    ,4, k.n0);
		k.n0 = hash64 ((char *)&m_q.m_qterms[i].m_termSign  ,1, k.n0);
		k.n0 = hash64 ((char *)&m_q.m_qterms[i].m_userWeight,4, k.n0);
		k.n0 = hash64 ((char *)&m_q.m_qterms[i].m_userType  ,1, k.n0);
	}
	// space separated, NULL terminated, list of meta tag names to display
	if ( m_displayMetas          ) 
		k.n0 = hash64b ( m_displayMetas          , k.n0 );

	// . now include the hash of the search parameters
	char *a = ((char *)&m_START) + 4 ; // msg40->m_dpf;
	char *b =  (char *)&m_END_HASH   ; // msg40->m_topicGroups;
	int32_t size = b - a; 
	// and hash it all up
	k.n0 = hash64 ( a , size , k.n0 );
	// . boolean queries have operators (AND OR NOT ( ) ) that we need
	//   to consider in this hash as well. so
	// . so just hash the whole damn query
	if ( m_q.m_isBoolean ) {
		char *q    = m_q.getQuery();
		int32_t  qlen = m_q.getQueryLen();
		k.n0 = hash64 ( q , qlen , k.n0 );
	}

	// Language stuff
	//k.n0 = hash64(m_defaultSortLanguage, m_defaultSortLanguageLen, k.n0);
	//k.n0 = hash64(m_defaultSortCountry , m_defaultSortCountryLen , k.n0);

	// debug
	//logf(LOG_DEBUG,"query: q=%s k.n0=%"UINT64"",m_q.getQuery(),k.n0);

	//Msg1aParms* m1p = msg40->getReferenceParms();
	//if( m1p ) {
	//	k.n0=hash64(((char*)m1p)+sizeof(int32_t), 
	//		    sizeof(Msg1aParms)-8,k.n0);
	//}
	return k;
}

void SearchInput::test ( ) {
	// set all to 0 just to avoid any inconsistencies
	char *a = ((char *)&m_START) + 4 ; // msg40->m_dpf;
	char *b =  (char *)&m_END_TEST;
	int32_t size = b - a;
	memset ( a , 0x00 , size );
	// loop through all possible cgi parms to set SearchInput
	for ( int32_t i = 0 ; i < g_parms.m_numSearchParms ; i++ ) {
		Parm *m = g_parms.m_searchParms[i];
		char *x = (char *)this + m->m_off;
		if ( m->m_type != TYPE_BOOL ) *(int32_t *)x = 0xffffffff;
		else                          *(char *)x = 0xff;
	}
	// ensure we're all zeros now!
	int32_t fix = a - (char *)this;
	unsigned char *p = (unsigned char *)a;
	for ( int32_t i = 0 ; i < size ; i++ ) {
		if ( p[i] == 0xff ) continue;
		// find it
		int32_t off = i + fix;
		char *name = NULL; // "unknown";
		for ( int32_t k = 0 ; k < g_parms.m_numSearchParms ; k++ ) {
			Parm *m = g_parms.m_searchParms[k];
			if ( m->m_off != off ) continue;
			name = m->m_title;
			break;
		}
		if ( ! name ) continue;
		log("query: Got uncovered SearchInput parm at offset "
		    "%"INT32" in SearchInput. name=%s.",off,name);
	}
}

void SearchInput::copy ( class SearchInput *si ) {
	gbmemcpy ( (char *)this , (char *)si , sizeof(SearchInput) );
}

bool SearchInput::set ( TcpSocket *sock , HttpRequest *r ) { //, Query *q ) {

	// store list of collection #'s to search here. usually just one.
	m_collnumBuf.reset();

	m_q.reset();

	// zero out everything, set niceness to 0
	clear ( 0 ) ;

	// save it now
	m_sock = sock;

	// still his buffer. m_hr will free the stuff, but "r" can
	// still access it for the time being, and not free it
	m_hr.stealBuf ( r );

	char *coll = g_collectiondb.getDefaultColl ( r );

	//////
	//
	// build "m_collnumBuf" to consist of all the collnums we should
	// be searching.
	//
	///////

	m_firstCollnum = -1;
	// set this to the collrec of the first valid collnum we encounter
	CollectionRec *cr = NULL;
	// now convert list of space-separated coll names into list of collnums
	char *p = r->getString("c",NULL);
	// if no collection list was specified look for "token=" and
	// use those to make collections. hack for diffbot.
	char *token = r->getString("token",NULL);
	// find all collections under this token
	for ( int32_t i = 0 ; i < g_collectiondb.m_numRecs ; i++ ) {
		// must not have a "&c="
		if ( p ) break;
		// must have a "&token="
		if ( ! token ) break;
		// skip if empty
		CollectionRec *tmpcr = g_collectiondb.m_recs[i];
		if ( ! tmpcr ) continue;
		// skip if does not match token
		if ( strcmp(token,tmpcr->m_diffbotToken.getBufStart()) ) 
			continue;
		// . we got a match
		// . set initial junk
		if ( ! cr ) {
			cr = tmpcr;
			m_firstCollnum = tmpcr->m_collnum;
		}
		// save the collection #
		if ( ! m_collnumBuf.safeMemcpy ( &tmpcr->m_collnum, 
						 sizeof(collnum_t) ) )
			return false;
	}

	// if we had a "&c=..." in the GET request process that
	if ( p ) {
	loop:
		char *end = p;
		for ( ; *end && ! is_wspace_a(*end) ; end++ );
		// temp null
		char c = *end;
		*end = '\0';
		CollectionRec *tmpcr = g_collectiondb.getRec ( p );
		// set defaults from the FIRST one
		if ( tmpcr && ! cr ) {
			cr = tmpcr;
			m_firstCollnum = tmpcr->m_collnum;
		}
		if ( ! tmpcr ) { 
			g_errno = ENOCOLLREC;
			log("query: missing collection %s",p);
			g_msg = " (error: no such collection)";		
			return false;
		}
		// add to our list
		if (!m_collnumBuf.safeMemcpy(&tmpcr->m_collnum,
					     sizeof(collnum_t)))
			return false;
		// restore the \0 character we wrote in there
		*end = c;
		// advance
		p = end;
		// skip to next collection name if there is one
		while ( *p && is_wspace_a(*p) ) p++; 
		// now add it's collection # to m_collnumBuf if there
		if ( *p ) goto loop;
	}

	// use default collection if none provided
	if ( ! p && ! token && m_collnumBuf.length() <= 0 ) {
		// get default collection rec
		cr = g_collectiondb.getRec (coll);
		// add to our list
		if ( cr &&
		     !m_collnumBuf.safeMemcpy(&cr->m_collnum,
					      sizeof(collnum_t)))
			return false;
	}
		


	/////
	//
	// END BUILDING m_collnumBuf
	//
	/////


	// save the collrec
	m_cr = cr;

	// must have had one
	if ( ! cr ) {
		log("si: si. collection does not exist");
		// if we comment the below out then it cores in setToDefault!
		g_errno = ENOCOLLREC;
		return false;
	}

	// and set from the http request. will set m_coll, etc.
	g_parms.setToDefault ( (char *)this , OBJ_SI , cr );


	///////
	//
	// set defaults of some things based on format language
	//
	//////

	// get the format. "xml" "html" "json" --> FORMAT_HTML, FORMAT_CSV ...
	char tmpFormat = m_hr.getReplyFormat();
	// now override automatic defaults for special cases
	if ( tmpFormat != FORMAT_HTML ) {
		m_familyFilter            = 0;
		m_doQueryHighlighting     = 0;
		m_getDocIdScoringInfo = false;
	}

	// if they have a list of sites...
	if ( m_sites && m_sites[0] ) {
		m_doSiteClustering        = false;
	}


	


	// and set from the http request. will set m_coll, etc.
	g_parms.setFromRequest ( &m_hr , sock , cr , (char *)this , OBJ_SI );

	if ( m_streamResults &&
	     tmpFormat != FORMAT_XML &&
	     tmpFormat != FORMAT_CSV &&
	     tmpFormat != FORMAT_JSON ) {
		log("si: streamResults only supported for "
		    "xml/csv/json. disabling");
		m_streamResults = false;
	}

	m_coll = coll;

	// it sets m_formatStr above, but we gotta set this...
	m_format = tmpFormat;


	//////
	//
	// fix some parms
	//
	//////

	// set m_isMasterAdmin to zero if no correct ip or password
	if ( ! g_conf.isMasterAdmin ( sock , &m_hr ) ) {
		m_isMasterAdmin = 0;
	}

	// collection admin?
	m_isCollAdmin = g_conf.isCollAdmin ( sock , &m_hr );

	//////////////////////////////////////
	//
	// transform input into classes
	//
	//////////////////////////////////////

	// allow for "qlang" if still don't have it
	//int32_t gglen2;
	//char *gg2 = r->getString ( "qlang" , &gglen2 , NULL );
	//if ( m_gblang == 0 && gg2 && gglen2 > 1 )
	//	m_gblang = getLanguageFromAbbr(gg2);
	
	// fix query by removing lang:xx from ask.com queries
	//char *end = m_query + m_queryLen -8;
	//if ( m_queryLen > 8 && m_query && end > m_query && 
	//     strncmp(end," lang:",6)==0 ) {
	//	char *asklang = m_query+m_queryLen - 2;
	//	m_gblang = getLanguageFromAbbr(asklang);
	//	m_queryLen -= 8;
	//	m_query[m_queryLen] = 0;
	//	
	//}

	// . returns false and sets g_errno on error
	// . sets m_qbuf1 and m_qbuf2
	// . sets:
	//   m_sbuf1
	//   m_sbuf2
	//   m_sbuf3
	//   m_displayQuery
	//   m_qe (encoded query)
	//   m_rtl (right to left like hebrew)
	//   m_highlightQuery
	if ( ! setQueryBuffers (r) ) {
		return log("query: setQueryBuffers: %s",mstrerror(g_errno));
	}


	// this overrides though
	//int32_t qlen2;
	//char *qs2 = r->getString ("qlang",&qlen2,NULL);
	//if ( qs2 ) qs1 = qs2;
	
	//m_queryLang = getLanguageFromAbbr ( qs1 );

	//m_queryLang = detectQueryLanguage();

	//char *qs1 = getLanguageAbbr(m_queryLang);

	// this parm is in Parms.cpp and should be set
	const char *langAbbr = m_defaultSortLang;

	// Parms.cpp sets it to an empty string, so make that null
	// if Parms.cpp set it to NULL it seems it comes out as "(null)"
	// i guess because we sprintf it or something.
	if ( langAbbr && langAbbr[0] == '\0' ) {
		langAbbr = NULL;
	}

	// detect language
	if ( !langAbbr ) {
		// detect language hints

		// language tag format:
		//   Language-Tag = Primary-tag *( "-" Subtag )
		//   Primary-tag = 1*8ALPHA
		//   Subtag = 1*8ALPHA
		char content_language_hint[64] = {}; // HTTP header Content-Language: field
		const char* tld_hint = NULL; // hostname of a URL

		bool valid_qlang = false;
		{
			const char* qlang = r->getString("fx_qlang");
			if (qlang) {
				// validate lang
				if (strlen(qlang) == 2) {
					valid_qlang = true;
					strcat(content_language_hint, qlang);
				}
			}
		}

		// only use other hints if fx_qlang is not set
		if (!valid_qlang) {
			const char* blang = r->getString("fx_blang");
			if (blang) {
				// validate lang
				size_t len = strlen(blang);
				if (len > 0 && len <= 17) {
					strcat(content_language_hint, blang);
				}
			}

			// use fx_fetld if available; if not, try with fx_country
			const char* fe_domain = r->getString("fx_fetld");
			if (fe_domain) {
				const char *pos = strrchr(fe_domain, '.');
				if (pos) {
					if (((fe_domain + strlen(fe_domain)) - pos) == 3) {
						tld_hint = pos + 1;
					}
				}
			}

			if (!tld_hint) {
				tld_hint = r->getString("fx_country");
			}
		}

		int encoding_hint = CLD2::UNKNOWN_ENCODING; // encoding detector applied to the input document
		CLD2::Language language_hint = CLD2::UNKNOWN_LANGUAGE; // any other context

		//log("query: cld2: using content_language_hint='%s' tld_hint='%s'", content_language_hint, tld_hint);

		CLD2::CLDHints cldhints = {content_language_hint, tld_hint, encoding_hint, language_hint};

		int flags = 0;
		flags |= CLD2::kCLDFlagBestEffort;

		// this is initialized by CLD2 library
		CLD2::Language language3[3];
		int percent3[3];
		double normalized_score3[3];

		CLD2::ResultChunkVector *resultchunkvector = NULL;

		int text_bytes = 0;
		bool is_reliable = false;
		int valid_prefix_bytes = 0;

		CLD2::Language language = CLD2::ExtDetectLanguageSummaryCheckUTF8(m_sbuf1.getBufStart(),
		                                                                  m_sbuf1.length(),
		                                                                  true,
		                                                                  &cldhints,
		                                                                  flags,
		                                                                  language3,
		                                                                  percent3,
		                                                                  normalized_score3,
		                                                                  resultchunkvector,
		                                                                  &text_bytes,
		                                                                  &is_reliable,
		                                                                  &valid_prefix_bytes);

		//log("query: cld2: lang0: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[0]), percent3[0], normalized_score3[0]);
		//log("query: cld2: lang1: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[1]), percent3[1], normalized_score3[1]);
		//log("query: cld2: lang2: %s(%d%% %3.0fp)", CLD2::LanguageCode(language3[2]), percent3[2], normalized_score3[2]);

		if (language != CLD2::UNKNOWN_LANGUAGE) {
			langAbbr = CLD2::LanguageCode(language);
		}
	}

	// if &qlang was not given explicitly fall back to coll rec
	if (cr && !langAbbr) {
		langAbbr = cr->m_defaultSortLanguage2;
	}

	// if no coll rec use language unknown
	if (!langAbbr) {
		langAbbr = "xx";
	}

	log(LOG_INFO,"query: using default lang of %s", langAbbr );

	// get code
	m_queryLangId = getLangIdFromAbbr ( langAbbr );

	// allow for 'xx', which means langUnknown
	if ( m_queryLangId == langUnknown &&
	     langAbbr &&
	     langAbbr[0] &&
	     langAbbr[0]!='x' ) {
		log("query: langAbbr of '%s' is NOT SUPPORTED. using langUnknown, 'xx'.", langAbbr);
	}

	int32_t maxQueryTerms = cr->m_maxQueryTerms;

	// . the query to use for highlighting... can be overriden with "hq"
	// . we need the language id for doing synonyms
	if ( m_prepend && m_prepend[0] )
		m_hqq.set2 ( m_prepend , m_queryLangId , true ,maxQueryTerms);
	else if ( m_highlightQuery && m_highlightQuery[0] )
		m_hqq.set2 (m_highlightQuery,m_queryLangId,true,maxQueryTerms);
	else if ( m_query && m_query[0] )
		m_hqq.set2 ( m_query , m_queryLangId , true,maxQueryTerms);

	// log it here
	log(LOG_INFO, "query: got query %s (len=%i)" ,m_sbuf1.getBufStart() ,m_sbuf1.length());

	// . now set from m_qbuf1, the advanced/composite query buffer
	// . returns false and sets g_errno on error (ETOOMANYOPERANDS)
	if ( ! m_q.set2 ( m_sbuf1.getBufStart(), 
			  m_queryLangId , 
			  m_queryExpansion ,
			  true , // use QUERY stopwords?
			  maxQueryTerms ) ) {
		g_msg = " (error: query has too many operands)";
		return false;
	}

	m_q.m_containingParent = (void *)this;

	if ( m_q.m_truncated && m_q.m_isBoolean ) {
		g_errno = EQUERYTOOBIG;
		g_msg = " (error: query is too long)";
		return false;
	}


	if ( m_hideAllClustered )
		m_doSiteClustering = true;

	// turn off some parms
	if ( m_q.m_hasPositiveSiteField ) {
		m_doSiteClustering    = false;
	}

	if ( m_q.m_hasQuotaField ) {
		m_doSiteClustering    = false;
		m_doDupContentRemoval = false;
	}

	if ( ! m_doSiteClustering )
		m_hideAllClustered = false;

	// sanity check
	if(m_firstResultNum < 0) {
		m_firstResultNum = 0;
	}

	// . if query has url: or site: term do NOT use cache by def.
	// . however, if spider is off then use the cache by default
	if ( m_useCache == -1 && g_conf.m_spideringEnabled ) {
		if      ( m_q.m_hasPositiveSiteField ) m_useCache = 0;
		else if ( m_q.m_hasIpField   ) m_useCache = 0;
		else if ( m_q.m_hasUrlField  ) m_useCache = 0;
		else if ( m_sites && m_sites[0] ) m_useCache = 0;
		//else if ( m_whiteListBuf.length() ) m_useCache = 0;
		else if ( m_url && m_url[0]   ) m_useCache = 0;
	}

	// if useCache is still -1 then turn it on
	if ( m_useCache == -1 ) m_useCache = 1;

	// never use cache if doing a rerank (msg3b)
	//if ( m_rerankRuleset >= 0 ) m_useCache = 0;
	bool readFromCache = false;
	if ( m_useCache ==  1  ) readFromCache = true;
	if ( m_rcache   ==  0  ) readFromCache = false;
	if ( m_useCache ==  0  ) readFromCache = false;
	// if useCache is false, don't write to cache if it was not specified
	if ( m_wcache == -1 ) {
		if ( m_useCache ==  0 ) m_wcache = 0;
		else                    m_wcache = 1;
	}
	// save it
	m_rcache = readFromCache;

	return true;
}

// . sets m_qbuf1[] and m_qbuf2[]
// . m_qbuf1[] is the advanced query
// . m_qbuf2[] is the query to be used for spell checking
// . returns false and set g_errno on error
bool SearchInput::setQueryBuffers ( HttpRequest *hr ) {

	m_sbuf1.reset();
	m_sbuf2.reset();

	int16_t qcs = csUTF8;
	if (m_queryCharset && m_queryCharset[0]){
		// we need to convert the query string to utf-8
		int32_t qclen = gbstrlen(m_queryCharset);
		qcs = get_iana_charset(m_queryCharset, qclen );
		if (qcs == csUnknown) {
			//g_errno = EBADCHARSET;
			//g_msg = "(error: unknown query charset)";
			//return false;
			qcs = csUTF8;
		}
	}
	// prepend sites terms
	int32_t numSites = 0;
	char *csStr = NULL;
	numSites = 0;
	csStr = get_charset_str(qcs);

	/*
	if ( m_sites && m_sites[0] ) {
		char *s = m_sites;
		char *t;
		int32_t  len;
		m_sbuf1.pushChar('(');// *p++ = '(';
	loop:
		// skip white space
		while ( *s && ! is_alnum_a(*s) ) s++;
		// bail if done
		if ( ! *s ) goto done;
		// get length of it
		t = s;
		while ( *t && ! is_wspace_a(*t) ) t++;
		len = t - s;
		// add site: term
		//if ( p + 12 + len >= pend ) goto toobig;
		if ( numSites > 0 ) m_sbuf1.safeStrcpy ( " UOR " );
		m_sbuf1.safeStrcpy ( "site:" );
		//p += ucToUtf8(p, pend-p,s, len, csStr, 0,0);
		m_sbuf1.safeMemcpy ( s , len );
		//gbmemcpy ( p , s , len     ); p += len;
		// *p++ = ' ';
		m_sbuf1.pushChar(' ');
		s = t;
		numSites++;
		goto loop;
	done:
		m_sbuf1.safePrintf(") | ");
		// inc totalLen
		m_sitesQueryLen = m_sitesLen + (numSites * 10);
	}
	*/

	// prepend
	char *qp = hr->getString("prepend",NULL,NULL);
	if( qp && qp[0] ) {
		//if( p > pstart ) *p++ =  ' ';
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		//p += sprintf( p, "+gblang:%"INT32" |", m_gblang );
		m_sbuf1.safePrintf( "%s", qp );
	}

	// boolean OR terms
	bool boolq = false;
	char *any = hr->getString("any",NULL);
	bool first = true;
	if ( any ) {
		char *s = any;
		char *send = any + gbstrlen(any);
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}
			if ( first ) m_sbuf1.safeStrcpy("(");
			if ( first ) m_sbuf2.safeStrcpy("(");
			if ( ! first ) m_sbuf1.safeStrcpy(" OR ");
			if ( ! first ) m_sbuf2.safeStrcpy(" OR ");
			first = false;
			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );
			s = s2 + 1;
		}
	}
	if ( ! first ) m_sbuf1.safeStrcpy(") AND ");
	if ( ! first ) m_sbuf2.safeStrcpy(") AND ");
	if ( ! first ) boolq = true;



	// and this
	if ( m_secsBack > 0 ) {
		int32_t timestamp = getTimeGlobalNoCore();
		timestamp -= m_secsBack;
		if ( timestamp <= 0 ) timestamp = 0;
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbminint:gbspiderdate:%"UINT32"",timestamp);
	}

	if ( m_sortBy == 1 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbsortbyint:gbspiderdate");
	}

	if ( m_sortBy == 2 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("gbrevsortbyint:gbspiderdate");
	}

	char *ft = m_filetype;
	if ( ft && strcasecmp(ft,"any")==0 ) ft = NULL;
	if ( ft && ! ft[0] ) ft = NULL;
	if ( ft ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf("filetype:%s",ft);
	}

	if ( m_familyFilter ) {
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	m_sbuf1.safePrintf( "+gbisadult:0");
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" |");
		}
		else {
			m_sbuf1.safeStrcpy(" AND ");
		}

	}

	// PRE-pend gblang: term
	int32_t gblang = hr->getLong("gblang",-1);
	if( gblang >= 0 ) {
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
	 	m_sbuf1.safePrintf( "+gblang:%"INT32"", gblang );
	 	m_sbuf2.safePrintf( "+gblang:%"INT32"", gblang );
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" |");
			m_sbuf2.safeStrcpy(" |");
		}
		else {
			m_sbuf1.safeStrcpy(" AND ");
			m_sbuf2.safeStrcpy(" AND ");
		}
	}

	// append url: term
	if ( m_link && m_link[0] ) {
	 	if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
	 	if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		m_sbuf1.safeStrcpy ( "+link:");
		m_sbuf2.safeStrcpy ( "+link:");
		m_sbuf1.safeStrcpy ( m_link );
		m_sbuf2.safeStrcpy ( m_link );
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" |");
			m_sbuf2.safeStrcpy(" |");
		}
		else {
			m_sbuf1.safeStrcpy(" AND ");
			m_sbuf2.safeStrcpy(" AND ");
		}
	}
	m_sbuf1.setLabel("sisbuf1");
	m_sbuf2.setLabel("sisbuf2");
	// append the natural query
	if ( m_query && m_query[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safeStrcpy ( m_query );
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		m_sbuf2.safeStrcpy ( m_query );
	}

	// append quoted phrases to query
	if ( m_quote1 && m_quote1[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" +\"");
			m_sbuf2.safeStrcpy(" +\"");
		}
		else {
			m_sbuf1.safeStrcpy(" AND \"");
			m_sbuf2.safeStrcpy(" AND \"");
		}
		m_sbuf1.safeStrcpy ( m_quote1 );
		m_sbuf1.safeStrcpy("\"");

		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');

		m_sbuf2.safeStrcpy ( m_quote1 );
		m_sbuf2.safeStrcpy("\"");
	}

	if ( m_quote2 && m_quote2[0] ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');

		if ( ! boolq ) {
			m_sbuf1.safeStrcpy(" +\"");
			m_sbuf2.safeStrcpy(" +\"");
		}
		else {
			m_sbuf1.safeStrcpy(" AND \"");
			m_sbuf2.safeStrcpy(" AND \"");
		}

		m_sbuf1.safeStrcpy ( m_quote2 );
		m_sbuf1.safeStrcpy("\"");

		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');

		m_sbuf2.safeStrcpy ( m_quote2 );
		m_sbuf2.safeStrcpy("\"");
	}

	// append plus terms
	if ( m_plus && m_plus[0] ) {
		char *s = m_plus;
		char *send = m_plus + gbstrlen(m_plus);

		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}

			if ( ! boolq ) {
				m_sbuf1.safeStrcpy("+");
				m_sbuf2.safeStrcpy("+");
			}
			else {
				m_sbuf1.safeStrcpy(" AND ");
				m_sbuf2.safeStrcpy(" AND ");
			}

			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );

			s = s2 + 1;
			if (s < send) {
				if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
				if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
			}
		}

	}  
	// append minus terms
	if ( m_minus && m_minus[0] ) {
		char *s = m_minus;
		char *send = m_minus + gbstrlen(m_minus);
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
		while (s < send) {
			while (isspace(*s) && s < send) s++;
			char *s2 = s+1;
			if (*s == '\"') {
				// if there's no closing quote just treat
				// the end of the line as such
				while (*s2 != '\"' && s2 < send) s2++;
				if (s2 < send) s2++;
			} else {
				while (!isspace(*s2) && s2 < send) s2++;
			}
			if (s2 < send) break;

			if ( ! boolq ) {
				m_sbuf1.safeStrcpy("-");
				m_sbuf2.safeStrcpy("-");
			}
			else {
				m_sbuf1.safeStrcpy(" AND NOT ");
				m_sbuf2.safeStrcpy(" AND NOT ");
			}

			m_sbuf1.safeMemcpy ( s , s2 - s );
			m_sbuf2.safeMemcpy ( s , s2 - s );

			s = s2 + 1;
			if (s < send) {
				if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
				if ( m_sbuf2.length() ) m_sbuf2.pushChar(' ');
			}
		}
	}
	// append gbkeyword:numinlinks if they have &mininlinks=X, X>0
	int32_t minInlinks = m_hr.getLong("mininlinks",0);
	if ( minInlinks > 0 ) {
		if ( m_sbuf1.length() ) m_sbuf1.pushChar(' ');
		m_sbuf1.safePrintf ( "gbkeyword:numinlinks");
	}

	// null terms
	if ( ! m_sbuf1.nullTerm() ) return false;
	if ( ! m_sbuf2.nullTerm() ) return false;

	// the natural query
	m_displayQuery = m_sbuf2.getBufStart();// + displayQueryOffset;

	if ( ! m_displayQuery ) m_displayQuery = "";

	while ( *m_displayQuery == ' ' ) m_displayQuery++;

	// urlencoded display query
	m_qe.urlEncode ( m_displayQuery );

	return true;
}
