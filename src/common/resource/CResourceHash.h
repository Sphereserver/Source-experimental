/**
* @file CResourceHash.h
*
*/

#ifndef _INC_CRESOURCEHASH_H
#define _INC_CRESOURCEHASH_H

#include "../sphere_library/CSObjSortArray.h"
#include "CResourceID.h"

class CResourceDef;


class CResourceHashArray : public CSObjSortArray< CResourceDef*, CResourceIDBase >
{
    // This list OWNS the CResourceDef and CResourceLink objects.
    // Sorted array of RESOURCE_ID
public:
    static const char *m_sClassName;
    CResourceHashArray() { }
private:
    CResourceHashArray(const CResourceHashArray& copy);
    CResourceHashArray& operator=(const CResourceHashArray& other);
public:
    int CompareKey( CResourceIDBase rid, CResourceDef * pBase, bool fNoSpaces ) const;
};

class CResourceHash
{
public:
    static const char *m_sClassName;
    CResourceHashArray m_Array[16];
public:
    CResourceHash()
    {
    }
private:
    CResourceHash(const CResourceHash& copy);
    CResourceHash& operator=(const CResourceHash& other);
private:
    int GetHashArray(const CResourceIDBase& rid) const
    {
        return (rid.GetResIndex() & 0x0F);
    }
public:
    inline size_t BadIndex() const
    {
        return m_Array[0].BadIndex();
    }
    size_t FindKey(const CResourceIDBase& rid) const;
    CResourceDef* GetAt(const CResourceIDBase& rid, size_t index) const;
    size_t AddSortKey(const CResourceIDBase& rid, CResourceDef* pNew);
    void SetAt(const CResourceIDBase& rid, size_t index, CResourceDef* pNew);
};


#endif // _INC_CRESOURCEHASH_H
