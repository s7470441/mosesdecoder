#pragma once

#include <vector>
#include "../../moses/src/TargetPhraseCollection.h"
#include "TargetPhrase.h"

namespace MosesBerkeleyPt
{
	
class TargetPhraseCollection
{
protected:
	typedef std::vector<const TargetPhrase*> CollType;
	CollType m_coll;

	Moses::TargetPhraseCollectionOtherInfo m_otherInfo;
		
	char *WriteToMemory(size_t &totalMemUsed, int numScores, size_t sourceWordSize, size_t targetWordSize) const;
public:
	typedef CollType::iterator iterator;
	typedef CollType::const_iterator const_iterator;
	//! iterators
	const_iterator begin() const { return m_coll.begin(); }
	const_iterator end() const { return m_coll.end(); }

	TargetPhraseCollection()
	{}
	TargetPhraseCollection(const TargetPhraseCollection &copy)
	{
		assert(copy.m_coll.size() == 0);
	}
	~TargetPhraseCollection();

	void AddTargetPhrase(const TargetPhrase *phrase)
	{
		m_coll.push_back(phrase);
	}

	size_t GetSize() const
	{ return m_coll.size(); }
	
	void Save(Db &db, long sourceNodeId, int numScores, size_t sourceWordSize, size_t targetWordSize) const;

	void SetOtherInfo(const Moses::TargetPhraseCollectionOtherInfo &otherInfo)
	{
		m_otherInfo = otherInfo;
	}
	
	const Moses::TargetPhraseCollectionOtherInfo &GetOtherInfo() const
	{ return m_otherInfo; }
	
};

};
