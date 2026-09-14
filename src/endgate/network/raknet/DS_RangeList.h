/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/// \file DS_RangeList.h
/// \internal
/// \brief A queue implemented as a linked list.
///

#ifndef __RANGE_LIST_H
#define __RANGE_LIST_H

#include <algorithm>
#include <vector>

#include "BitStream.h"
#include "RakMemoryOverride.h"
#include "RakNetDefines.h"

namespace DataStructures {
template <class range_type>
struct RangeNode {
    RangeNode() {}
    ~RangeNode() {}
    RangeNode(range_type min, range_type max)
    {
        minIndex = min;
        maxIndex = max;
    }
    range_type minIndex;
    range_type maxIndex;
};

template <class range_type>
class RangeList {
public:
    ~RangeList();
    void Insert(range_type index);
    void Clear(void);
    unsigned Size(void) const;
    unsigned RangeSum(void) const;
    RakNet::BitSize_t Serialize(RakNet::BitStream *in, RakNet::BitSize_t maxBits, bool clearSerialized);
    bool Deserialize(RakNet::BitStream *out);

    std::vector<RangeNode<range_type>> ranges;
};

template <class range_type>
RakNet::BitSize_t RangeList<range_type>::Serialize(RakNet::BitStream *in, RakNet::BitSize_t maxBits,
                                                   bool clearSerialized)
{
    RakAssert(ranges.size() < (unsigned short)-1);
    RakNet::BitStream tempBS;
    RakNet::BitSize_t bitsWritten;
    unsigned short countWritten;
    unsigned i;
    countWritten = 0;
    bitsWritten = 0;
    for (i = 0; i < ranges.size(); i++) {
        if ((int)sizeof(unsigned short) * 8 + bitsWritten + (int)sizeof(range_type) * 8 * 2 + 1 > maxBits) {
            break;
        }
        unsigned char minEqualsMax;
        if (ranges[i].minIndex == ranges[i].maxIndex) {
            minEqualsMax = 1;
        }
        else {
            minEqualsMax = 0;
        }
        tempBS.Write(minEqualsMax);  // Use one byte, intead of one bit, for speed, as this is done a lot
        tempBS.Write(ranges[i].minIndex);
        bitsWritten += sizeof(range_type) * 8 + 8;
        if (ranges[i].minIndex != ranges[i].maxIndex) {
            tempBS.Write(ranges[i].maxIndex);
            bitsWritten += sizeof(range_type) * 8;
        }
        countWritten++;
    }

    in->AlignWriteToByteBoundary();
    RakNet::BitSize_t before = in->GetWriteOffset();
    in->Write(countWritten);
    bitsWritten += in->GetWriteOffset() - before;
    in->Write(&tempBS, tempBS.GetNumberOfBitsUsed());

    if (clearSerialized && countWritten) {
        ranges.erase(ranges.begin(), ranges.begin() + countWritten);
    }

    return bitsWritten;
}
template <class range_type>
bool RangeList<range_type>::Deserialize(RakNet::BitStream *out)
{
    ranges.clear();
    unsigned short count;
    out->AlignReadToByteBoundary();
    out->Read(count);
    unsigned short i;
    range_type min, max;
    unsigned char maxEqualToMin = 0;

    for (i = 0; i < count; i++) {
        out->Read(maxEqualToMin);
        if (out->Read(min) == false) {
            return false;
        }
        if (maxEqualToMin == false) {
            if (out->Read(max) == false) {
                return false;
            }
            if (max < min) {
                return false;
            }
        }
        else {
            max = min;
        }

        ranges.push_back(RangeNode<range_type>(min, max));
    }
    return true;
}

template <class range_type>
RangeList<range_type>::~RangeList()
{
    Clear();
}

template <class range_type>
void RangeList<range_type>::Insert(range_type index)
{
    if (ranges.empty()) {
        ranges.push_back(RangeNode<range_type>(index, index));
        return;
    }

    auto insertionPoint =
        std::lower_bound(ranges.begin(), ranges.end(), index,
                         [](const RangeNode<range_type> &node, const range_type &key) {
                             return node.minIndex < key;
                         });
    unsigned insertionIndex = (unsigned)(insertionPoint - ranges.begin());
    if (insertionIndex == ranges.size()) {
        if (index == ranges[insertionIndex - 1].maxIndex + (range_type)1) {
            ranges[insertionIndex - 1].maxIndex++;
        }
        else if (index > ranges[insertionIndex - 1].maxIndex + (range_type)1) {
            // Insert at end
            ranges.push_back(RangeNode<range_type>(index, index));
        }

        return;
    }

    if (index < ranges[insertionIndex].minIndex - (range_type)1) {
        // Insert here
        ranges.insert(insertionPoint, RangeNode<range_type>(index, index));

        return;
    }
    else if (index == ranges[insertionIndex].minIndex - (range_type)1) {
        // Decrease minIndex and join left
        ranges[insertionIndex].minIndex--;
        if (insertionIndex > 0 &&
            ranges[insertionIndex - 1].maxIndex + (range_type)1 == ranges[insertionIndex].minIndex) {
            ranges[insertionIndex - 1].maxIndex = ranges[insertionIndex].maxIndex;
            ranges.erase(ranges.begin() + insertionIndex);
        }

        return;
    }
    else if (index >= ranges[insertionIndex].minIndex && index <= ranges[insertionIndex].maxIndex) {
        // Already exists
        return;
    }
    else if (index == ranges[insertionIndex].maxIndex + (range_type)1) {
        // Increase maxIndex and join right
        ranges[insertionIndex].maxIndex++;
        if (insertionIndex < ranges.size() - 1 &&
            ranges[insertionIndex + (range_type)1].minIndex == ranges[insertionIndex].maxIndex + (range_type)1) {
            ranges[insertionIndex + 1].minIndex = ranges[insertionIndex].minIndex;
            ranges.erase(ranges.begin() + insertionIndex);
        }

        return;
    }
}

template <class range_type>
void RangeList<range_type>::Clear(void)
{
    ranges.clear();
}

template <class range_type>
unsigned RangeList<range_type>::Size(void) const
{
    return (unsigned)ranges.size();
}

template <class range_type>
unsigned RangeList<range_type>::RangeSum(void) const
{
    unsigned sum = 0, i;
    for (i = 0; i < ranges.size(); i++) {
        sum += ranges[i].maxIndex - ranges[i].minIndex + 1;
    }
    return sum;
}

}  // namespace DataStructures

#endif
