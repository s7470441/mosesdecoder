// $Id$
// vim:tabstop=2

/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include <typeinfo>
#include <algorithm>
#include <typeinfo>
#include "TranslationOptionCollection.h"
#include "Sentence.h"
#include "DecodeStep.h"
#include "LM/Base.h"
#include "FactorCollection.h"
#include "InputType.h"
#include "Util.h"
#include "StaticData.h"
#include "DecodeStepTranslation.h"
#include "DecodeStepGeneration.h"
#include "DecodeGraph.h"
#include "InputPath.h"
#include "moses/FF/UnknownWordPenaltyProducer.h"
#include "moses/FF/LexicalReordering/LexicalReordering.h"
#include "moses/FF/InputFeature.h"
#include "util/exception.hh"

#include <boost/foreach.hpp>
using namespace std;

namespace Moses
{

/** helper for pruning */
// bool CompareTranslationOption(const TranslationOption *a, const TranslationOption *b)
// {
//   return a->GetFutureScore() > b->GetFutureScore();
// }

/** constructor; since translation options are indexed by coverage span, the
 * corresponding data structure is initialized here This fn should be
 * called by inherited classe */
TranslationOptionCollection::
TranslationOptionCollection(InputType const& src, size_t maxNoTransOptPerCoverage, 
			    float translationOptionThreshold)
  : m_source(src)
  , m_futureScore(src.GetSize())
  , m_maxNoTransOptPerCoverage(maxNoTransOptPerCoverage)
  , m_translationOptionThreshold(translationOptionThreshold)
{
  // create 2-d vector
  size_t size = src.GetSize();
  for (size_t sPos = 0 ; sPos < size ; ++sPos) {
    m_collection.push_back( vector< TranslationOptionList >() );

    size_t maxSize = size - sPos;
    size_t maxSizePhrase = StaticData::Instance().GetMaxPhraseLength();
    maxSize = std::min(maxSize, maxSizePhrase);

    for (size_t ePos = 0 ; ePos < maxSize ; ++ePos) {
      m_collection[sPos].push_back( TranslationOptionList() );
    }
  }
}

/** destructor, clears out data structures */
TranslationOptionCollection::
~TranslationOptionCollection()
{
  RemoveAllInColl(m_inputPathQueue);
}

void 
TranslationOptionCollection::
Prune()
{
  static float no_th = -std::numeric_limits<float>::infinity();

  if (m_maxNoTransOptPerCoverage == 0 && m_translationOptionThreshold == no_th)
    return;

  // bookkeeping for how many options used, pruned
  size_t total = 0;
  size_t totalPruned = 0;

  // loop through all spans
  size_t size = m_source.GetSize();
  for (size_t sPos = 0 ; sPos < size; ++sPos) 
    {
      BOOST_FOREACH(TranslationOptionList& fullList, m_collection[sPos])
	{
	  total       += fullList.size();
	  totalPruned += fullList.SelectNBest(m_maxNoTransOptPerCoverage);
	  totalPruned += fullList.PruneByThreshold(m_translationOptionThreshold);
        }
    }
  
  VERBOSE(2,"       Total translation options: " << total << std::endl
          << "Total translation options pruned: " << totalPruned << std::endl);
}

/** Force a creation of a translation option where there are none for a
* particular source position.  ie. where a source word has not been
* translated, create a translation option by
* 1. not observing the table limits on phrase/generation tables
* 2. using the handler ProcessUnknownWord()
* Call this function once translation option collection has been filled with
* translation options
*
* This function calls for unknown words is complicated by the fact it must
* handle different input types.  The call stack is 
* Base::ProcessUnknownWord() 
* Inherited::ProcessUnknownWord(position) 
* Base::ProcessOneUnknownWord()
*
*/

void 
TranslationOptionCollection::
ProcessUnknownWord()
{
  const vector<DecodeGraph*>& decodeGraphList 
    = StaticData::Instance().GetDecodeGraphs();
  size_t size = m_source.GetSize();
  // try to translation for coverage with no trans by expanding table limit
  for (size_t graphInd = 0 ; graphInd < decodeGraphList.size() ; graphInd++) {
    const DecodeGraph &decodeGraph = *decodeGraphList[graphInd];
    for (size_t pos = 0 ; pos < size ; ++pos) {
      TranslationOptionList* fullList = GetTranslationOptionList(pos, pos);
      // size_t numTransOpt = fullList.size();
      if (!fullList || fullList->size() == 0) {
        CreateTranslationOptionsForRange(decodeGraph, pos, pos, false, graphInd);
      }
    }
  }

  bool alwaysCreateDirectTranslationOption 
    = StaticData::Instance().IsAlwaysCreateDirectTranslationOption();
  // create unknown words for 1 word coverage where we don't have any trans options
  for (size_t pos = 0 ; pos < size ; ++pos) {
    TranslationOptionList* fullList = GetTranslationOptionList(pos, pos);
    if (!fullList || fullList->size() == 0 || alwaysCreateDirectTranslationOption)
      ProcessUnknownWord(pos);
  }
}

/** special handling of ONE unknown words. Either add temporarily add word to
 * translation table, or drop the translation.  This function should be
 * called by the ProcessOneUnknownWord() in the inherited class At the
 * moment, this unknown word handler is a bit of a hack, if copies over
 * each factor from source to target word, or uses the 'UNK' factor.
 * Ideally, this function should be in a class which can be expanded
 * upon, for example, to create a morphologically aware handler.
 *
 * \param sourceWord the unknown word
 * \param sourcePos
 * \param length length covered by this word (may be > 1 for lattice input)
 * \param inputScores a set of scores associated with unknown word (input scores from latties/CNs)
 */
void 
TranslationOptionCollection::
ProcessOneUnknownWord(const InputPath &inputPath, size_t sourcePos,
		      size_t length, const ScorePair *inputScores)
{
  const StaticData &staticData = StaticData::Instance();
  const UnknownWordPenaltyProducer& 
    unknownWordPenaltyProducer = UnknownWordPenaltyProducer::Instance();
  float unknownScore = FloorScore(TransformScore(0));
  const Word &sourceWord = inputPath.GetPhrase().GetWord(0);

  // hack. Once the OOV FF is a phrase table, get rid of this
  PhraseDictionary *firstPt = NULL;
  if (PhraseDictionary::GetColl().size() == 0) {
    firstPt = PhraseDictionary::GetColl()[0];
  }

  // unknown word, add as trans opt
  FactorCollection &factorCollection = FactorCollection::Instance();

  size_t isDigit = 0;

  const Factor *f = sourceWord[0]; // TODO hack. shouldn't know which factor is surface
  const StringPiece s = f->GetString();
  bool isEpsilon = (s=="" || s==EPSILON);
  if (StaticData::Instance().GetDropUnknown()) {


    isDigit = s.find_first_of("0123456789");
    if (isDigit == string::npos)
      isDigit = 0;
    else
      isDigit = 1;
    // modify the starting bitmap
  }

  TargetPhrase targetPhrase(firstPt);

  if (!(staticData.GetDropUnknown() || isEpsilon) || isDigit) {
    // add to dictionary

    Word &targetWord = targetPhrase.AddWord();
    targetWord.SetIsOOV(true);

    for (unsigned int currFactor = 0 ; currFactor < MAX_NUM_FACTORS ; currFactor++) {
      FactorType factorType = static_cast<FactorType>(currFactor);

      const Factor *sourceFactor = sourceWord[currFactor];
      if (sourceFactor == NULL)
        targetWord[factorType] = factorCollection.AddFactor(UNKNOWN_FACTOR);
      else
        targetWord[factorType] = factorCollection.AddFactor(sourceFactor->GetString());
    }
    //create a one-to-one alignment between UNKNOWN_FACTOR and its verbatim translation

    targetPhrase.SetAlignmentInfo("0-0");
    
  } 
  
  targetPhrase.GetScoreBreakdown().Assign(&unknownWordPenaltyProducer, unknownScore);

  // source phrase
  const Phrase &sourcePhrase = inputPath.GetPhrase();
  m_unksrcs.push_back(&sourcePhrase);
  WordsRange range(sourcePos, sourcePos + length - 1);

  targetPhrase.EvaluateInIsolation(sourcePhrase);

  TranslationOption *transOpt = new TranslationOption(range, targetPhrase);
  transOpt->SetInputPath(inputPath);
  Add(transOpt);


}

/** compute future score matrix in a dynamic programming fashion.
	* This matrix used in search.
	* Call this function once translation option collection has been filled with translation options
*/
void 
TranslationOptionCollection::
CalcFutureScore()
{
  // setup the matrix (ignore lower triangle, set upper triangle to -inf
  size_t size = m_source.GetSize(); // the width of the matrix

  for(size_t row=0; row < size; row++) {
    for(size_t col=row; col<size; col++) {
      m_futureScore.SetScore(row, col, -numeric_limits<float>::infinity());
    }
  }

  // walk all the translation options and record the cheapest option for each span
  for (size_t sPos = 0 ; sPos < size ; ++sPos) 
    {
      size_t ePos = sPos;
      BOOST_FOREACH(TranslationOptionList& tol, m_collection[sPos])
	{
	  TranslationOptionList::const_iterator toi;
	  for(toi = tol.begin() ; toi != tol.end() ; ++toi) {
	    const TranslationOption& to = **toi;
	    float score = to.GetFutureScore();
	    if (score > m_futureScore.GetScore(sPos, ePos))
	      m_futureScore.SetScore(sPos, ePos, score);
	  }
	  ++ePos;
	}
    }

  // now fill all the cells in the strictly upper triangle
  //   there is no way to modify the diagonal now, in the case
  //   where no translation option covers a single-word span,
  //   we leave the +inf in the matrix
  // like in chart parsing we want each cell to contain the highest score
  // of the full-span trOpt or the sum of scores of joining two smaller spans

  for(size_t colstart = 1; colstart < size ; colstart++) {
    for(size_t diagshift = 0; diagshift < size-colstart ; diagshift++) {
      size_t sPos = diagshift;
      size_t ePos = colstart+diagshift;
      for(size_t joinAt = sPos; joinAt < ePos ; joinAt++)  {
        float joinedScore = m_futureScore.GetScore(sPos, joinAt)
                            + m_futureScore.GetScore(joinAt+1, ePos);
        // uncomment to see the cell filling scheme
	// TRACE_ERR("[" << sPos << "," << ePos << "] <-? [" 
	// 	  << sPos << "," << joinAt << "]+[" 
	// 	  << joinAt+1 << "," << ePos << "] (colstart: " 
	// 	  << colstart << ", diagshift: " << diagshift << ")" 
	// 	  << endl);
	        
        if (joinedScore > m_futureScore.GetScore(sPos, ePos))
          m_futureScore.SetScore(sPos, ePos, joinedScore);
      }
    }
  }
  
  IFVERBOSE(3) 
    {
      int total = 0;
      for(size_t row = 0; row < size; row++) 
	{
	  size_t col = row;
	  BOOST_FOREACH(TranslationOptionList& tol, m_collection[row])
	    {
	      // size_t maxSize = size - row;
	      // size_t maxSizePhrase = StaticData::Instance().GetMaxPhraseLength();
	      // maxSize = std::min(maxSize, maxSizePhrase);

	      // for(size_t col=row; col<row+maxSize; col++) {
	      int count = tol.size();
	      TRACE_ERR( "translation options spanning from  "
			 << row <<" to "<< col <<" is "
			 << count <<endl);
	      total += count;
	      ++col;
	    }
	}
      TRACE_ERR( "translation options generated in total: "<< total << endl);
      
      for(size_t row=0; row<size; row++)
	for(size_t col=row; col<size; col++)
	  TRACE_ERR( "future cost from "<< row <<" to "<< col <<" is "
		     << m_futureScore.GetScore(row, col) <<endl);
    }
}



/** Create all possible translations from the phrase tables
 * for a particular input sentence. This implies applying all
 * translation and generation steps. Also computes future cost matrix.
 */
void 
TranslationOptionCollection::
CreateTranslationOptions()
{
  // loop over all substrings of the source sentence, look them up
  // in the phraseDictionary (which is the- possibly filtered-- phrase
  // table loaded on initialization), generate TranslationOption objects
  // for all phrases

  // there may be multiple decoding graphs (factorizations of decoding)
  const vector <DecodeGraph*> &decodeGraphList 
    = StaticData::Instance().GetDecodeGraphs();

  // length of the sentence
  const size_t size = m_source.GetSize();

  // loop over all decoding graphs, each generates translation options
  for (size_t gidx = 0 ; gidx < decodeGraphList.size() ; gidx++) 
    {
      if (decodeGraphList.size() > 1) 
	VERBOSE(3,"Creating translation options from decoding graph " << gidx << endl);
      
      const DecodeGraph& dg = *decodeGraphList[gidx];
      size_t backoff = dg.GetBackoff();
      // iterate over spans
      for (size_t sPos = 0 ; sPos < size; sPos++) 
	{
	  size_t maxSize = size - sPos; // don't go over end of sentence
	  size_t maxSizePhrase = StaticData::Instance().GetMaxPhraseLength();
	  maxSize = std::min(maxSize, maxSizePhrase);
	  
	  for (size_t ePos = sPos ; ePos < sPos + maxSize ; ePos++) 
	    {
	      if (gidx && backoff && 
		  (ePos-sPos+1 <= backoff || // size exceeds backoff limit (HUH? UG) or ...
		   m_collection[sPos][ePos-sPos].size() > 0))
		{ 
		  VERBOSE(3,"No backoff to graph " << gidx << " for span [" << sPos << ";" << ePos << "]" << endl);
		  continue;
		}
	      CreateTranslationOptionsForRange(dg, sPos, ePos, true, gidx);
	    }
	}
    }
  VERBOSE(3,"Translation Option Collection\n " << *this << endl);
  ProcessUnknownWord();
  EvaluateWithSourceContext();
  Prune();
  Sort();
  CalcFutureScore(); // future score matrix
  CacheLexReordering(); // Cached lex reodering costs
}


bool
TranslationOptionCollection::
CreateTranslationOptionsForRange
(const DecodeGraph& dg, size_t sPos, size_t ePos,  
 bool adhereTableLimit, size_t gidx, InputPath &inputPath)
{
  if ((StaticData::Instance().GetXmlInputType() != XmlExclusive) 
      || !HasXmlOptionsOverlappingRange(sPos,ePos)) 
    {

      // partial trans opt stored in here
      PartialTranslOptColl* oldPtoc = new PartialTranslOptColl;
      size_t totalEarlyPruned = 0;

      // initial translation step
      list <const DecodeStep* >::const_iterator iterStep = dg.begin();
      const DecodeStep &dstep = **iterStep;

      const PhraseDictionary &pdict = *dstep.GetPhraseDictionaryFeature();
      const TargetPhraseCollection *targetPhrases = inputPath.GetTargetPhrases(pdict);
    
      static_cast<const DecodeStepTranslation&>(dstep).ProcessInitialTranslation
	(m_source, *oldPtoc, sPos, ePos, adhereTableLimit, inputPath, targetPhrases);

      SetInputScore(inputPath, *oldPtoc);

      // do rest of decode steps
      int indexStep = 0;

      for (++iterStep ; iterStep != dg.end() ; ++iterStep) {
	
	const DecodeStep *dstep = *iterStep;
	PartialTranslOptColl* newPtoc = new PartialTranslOptColl;
	
	// go thru each intermediate trans opt just created
	const vector<TranslationOption*>& partTransOptList = oldPtoc->GetList();
	vector<TranslationOption*>::const_iterator iterPartialTranslOpt;
	for (iterPartialTranslOpt = partTransOptList.begin() ; iterPartialTranslOpt != partTransOptList.end() ; ++iterPartialTranslOpt) {
	  TranslationOption &inputPartialTranslOpt = **iterPartialTranslOpt;

	  if (const DecodeStepTranslation *tstep = dynamic_cast<const DecodeStepTranslation*>(dstep) ) 
	    {
	      const PhraseDictionary &pdict = *tstep->GetPhraseDictionaryFeature();
	      const TargetPhraseCollection *targetPhrases = inputPath.GetTargetPhrases(pdict);
	      tstep->Process(inputPartialTranslOpt, *dstep, *newPtoc, 
				     this, adhereTableLimit, targetPhrases);
	  } else {
	    const DecodeStepGeneration *genStep = dynamic_cast<const DecodeStepGeneration*>(dstep);
	    assert(genStep);
	    genStep->Process(inputPartialTranslOpt, *dstep, *newPtoc, 
			     this, adhereTableLimit);
	  }
	}
	
	// last but 1 partial trans not required anymore
	totalEarlyPruned += newPtoc->GetPrunedCount();
	delete oldPtoc;
	oldPtoc = newPtoc;
	
	indexStep++;
      } // for (++iterStep
      
      // add to fully formed translation option list
      PartialTranslOptColl &lastPartialTranslOptColl	= *oldPtoc;
      const vector<TranslationOption*>& partTransOptList = lastPartialTranslOptColl.GetList();
      vector<TranslationOption*>::const_iterator iterColl;
      for (iterColl = partTransOptList.begin() ; iterColl != partTransOptList.end() ; ++iterColl) {
	TranslationOption *transOpt = *iterColl;
	if (StaticData::Instance().GetXmlInputType() != XmlConstraint || !ViolatesXmlOptionsConstraint(sPos,ePos,transOpt)) {
        Add(transOpt);
      }
      
    lastPartialTranslOptColl.DetachAll();
    totalEarlyPruned += oldPtoc->GetPrunedCount();
    delete oldPtoc;
    // TRACE_ERR( "Early translation options pruned: " << totalEarlyPruned << endl);
    } // if ((StaticData::Instance().GetXmlInputType() != XmlExclusive) || !HasXmlOptionsOverlappingRange(sPos,ePos))
  
  if (gidx == 0 && StaticData::Instance().GetXmlInputType() != XmlPassThrough && HasXmlOptionsOverlappingRange(sPos,ePos)) {
    CreateXmlOptionsForRange(sPos, ePos);
  }

  return true;
}

void 
TranslationOptionCollection::
SetInputScore(const InputPath &inputPath, PartialTranslOptColl &oldPtoc)
{
  const ScorePair* inputScore = inputPath.GetInputScore();
  if (inputScore == NULL) return;

  const InputFeature &inputFeature = InputFeature::Instance();

  const std::vector<TranslationOption*> &transOpts = oldPtoc.GetList();
  for (size_t i = 0; i < transOpts.size(); ++i) {
    TranslationOption &transOpt = *transOpts[i];

    ScoreComponentCollection &scores = transOpt.GetScoreBreakdown();
    scores.PlusEquals(&inputFeature, *inputScore);

  }
}

void 
TranslationOptionCollection::
EvaluateWithSourceContext()
{
  const size_t size = m_source.GetSize();
  for (size_t sPos = 0 ; sPos < size ; ++sPos) 
    {
      BOOST_FOREACH(TranslationOptionList& tol, m_collection[sPos])
	{
	  typedef TranslationOptionList::const_iterator to_iter;
	  for(to_iter i = tol.begin() ; i != tol.end() ; ++i) 
	    (*i)->EvaluateWithSourceContext(m_source);
	}
    }
}

void 
TranslationOptionCollection::
Sort()
{
  static TranslationOption::Better cmp;
  size_t size = m_source.GetSize();
  for (size_t sPos = 0 ; sPos < size; ++sPos) 
    {
      BOOST_FOREACH(TranslationOptionList& tol, m_collection.at(sPos))
	std::sort(tol.begin(), tol.end(), cmp);
    }
}

/** Check if this range overlaps with any XML options. This doesn't need to be an exact match, only an overlap.
 * by default, we don't support XML options. subclasses need to override this function.
 * called by CreateTranslationOptionsForRange()
 * \param sPos first position in input sentence
 * \param lastPos last position in input sentence
 */
bool 
TranslationOptionCollection::
HasXmlOptionsOverlappingRange(size_t, size_t) const
{ return false; }

/** Check if an option conflicts with any constraint XML options. Okay, if XML option is substring in source and target.
 * by default, we don't support XML options. subclasses need to override this function.
 * called by CreateTranslationOptionsForRange()
 * \param sPos first position in input sentence
 * \param lastPos last position in input sentence
 */
bool 
TranslationOptionCollection::
ViolatesXmlOptionsConstraint(size_t, size_t, TranslationOption*) const
{ return false; }

/** Populates the current Collection with XML options exactly covering the range specified. Default implementation does nothing.
 * called by CreateTranslationOptionsForRange()
 * \param sPos first position in input sentence
 * \param lastPos last position in input sentence
 */
void 
TranslationOptionCollection::
CreateXmlOptionsForRange(size_t, size_t)
{ }


/** Add translation option to the list
 * \param translationOption translation option to be added */
void 
TranslationOptionCollection::
Add(TranslationOption *translationOption)
{
  const WordsRange &coverage = translationOption->GetSourceWordsRange();

  if (coverage.GetEndPos() - coverage.GetStartPos() >= m_collection[coverage.GetStartPos()].size()) {
	  cerr << "translationOption=" << *translationOption << endl;
	  cerr << "coverage=" << coverage << endl;
  }

  UTIL_THROW_IF2(coverage.GetEndPos() - coverage.GetStartPos() >= m_collection[coverage.GetStartPos()].size(),
		  "Out of bound access: " << coverage);
  m_collection[coverage.GetStartPos()][coverage.GetEndPos() - coverage.GetStartPos()].Add(translationOption);
}

TO_STRING_BODY(TranslationOptionCollection);

std::ostream& operator<<(std::ostream& out, const TranslationOptionCollection& coll)
{
  size_t size = coll.m_source.GetSize();
  for (size_t sPos = 0 ; sPos < size ; ++sPos) {
    size_t maxSize = size - sPos;
    size_t maxSizePhrase = StaticData::Instance().GetMaxPhraseLength();
    maxSize = std::min(maxSize, maxSizePhrase);

    for (size_t ePos = sPos ; ePos < sPos + maxSize ; ++ePos) {
      const TranslationOptionList* fullList 
	= coll.GetTranslationOptionList(sPos, ePos);
      if (!fullList) break;
      size_t sizeFull = fullList->size();
      for (size_t i = 0; i < sizeFull; i++) {
        out << *fullList->Get(i) << std::endl;
      }
    }
  }
  return out;
}

void 
TranslationOptionCollection::
CacheLexReordering()
{
  size_t size = m_source.GetSize();

  const std::vector<const StatefulFeatureFunction*> &ffs = StatefulFeatureFunction::GetStatefulFeatureFunctions();
  std::vector<const StatefulFeatureFunction*>::const_iterator iter;
  for (iter = ffs.begin(); iter != ffs.end(); ++iter) {
    const StatefulFeatureFunction &ff = **iter;
    if (typeid(ff) == typeid(LexicalReordering)) {
      const LexicalReordering &lexreordering = static_cast<const LexicalReordering&>(ff);
      for (size_t sPos = 0 ; sPos < size ; sPos++) {
        size_t maxSize =  size - sPos;
        size_t maxSizePhrase = StaticData::Instance().GetMaxPhraseLength();
        maxSize = std::min(maxSize, maxSizePhrase);

	BOOST_FOREACH(TranslationOptionList& tol, m_collection)
	  {
	    TranslationOptionList::iterator it;
	    for(it = tol.begin(); it != tol.end(); ++it) 
	    {
	      TranslationOption &o = **it;
	      const Phrase &srcPhrase = o.GetInputPath().GetPhrase();
	      Scores score = lexreordering.GetProb(srcPhrase, o.GetTargetPhrase());
	      if (!score.empty())
		transOpt.CacheLexReorderingScores(lexreordering, score);
	    } // for(iterTransOpt
        } // for (size_t ePos = sPos ; ePos < sPos + maxSize; ePos++) {
      } // for (size_t sPos = 0 ; sPos < size ; sPos++) {
    } // if (typeid(ff) == typeid(LexicalReordering)) {
  } // for (iter = ffs.begin(); iter != ffs.end(); ++iter) {
}

//! list of trans opt for a particular span
TranslationOptionList*
TranslationOptionCollection::
GetTranslationOptionList(size_t const sPos, size_t const ePos)
{
  UTIL_THROW_IF2(sPos >= m_collection.size(), "Out of bound access.");
  vector<TranslationOptionList>& tol = m_collection[sPos];
  size_t idx = ePos - sPos;
  return idx < tol.size() ? &tol[idx] : NULL;
}

TranslationOptionList const*
TranslationOptionCollection::
GetTranslationOptionList(size_t sPos, size_t ePos) const
{
  UTIL_THROW_IF2(sPos >= m_collection.size(), "Out of bound access.");
  vector<TranslationOptionList>& tol = m_collection[sPos];
  size_t idx = ePos - sPos;
  return idx < tol.size() ? &tol[idx] : NULL;
}

void 
TranslationOptionCollection::
GetTargetPhraseCollectionBatch()
{
  const vector <DecodeGraph*> &dgl = StaticData::Instance().GetDecodeGraphs();
  BOOST_FOREACH(DecodeGraph const& dg, dgl)
    {
      typedef list <const DecodeStep* >::const_iterator dsiter;
      for (dsiter i = dg.begin(); i != dg.end() ; ++i) 
	{
	  const DecodeStepTranslation* tstep = dynamic_cast<const DecodeStepTranslation *>(*i);
	  if (tstep) 
	    {
	      const PhraseDictionary &pdict = *tstep->GetPhraseDictionaryFeature();
	      pdict.GetTargetPhraseCollectionBatch(m_inputPathQueue);
	    }
	}
    }
}

} // namespace

