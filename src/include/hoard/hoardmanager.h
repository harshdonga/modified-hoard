#ifndef HOARD_HOARDMANAGER_H
#define HOARD_HOARDMANAGER_H

#include <cstdlib>
#include <new>
#include <mutex>
#include <fstream>
#include <unistd.h> // For sysconf(_SC_NPROCESSORS_ONLN)

// Hoard-specific Heap Layers
#include "statistics.h"
#include "emptyclass.h"
#include "array.h"
#include "manageonesuperblock.h"
#include "basehoardmanager.h"
#include "emptyhoardmanager.h"


#include "heaplayers.h"

using namespace HL;

namespace Hoard {

  template <class SourceHeap,
	    class ParentHeap,
	    class SuperblockType_,
	    int EmptinessClasses,
	    class LockType,
	    class thresholdFunctionClass,
	    class HeapType>
  class HoardManager : public BaseHoardManager<SuperblockType_>,
		       public thresholdFunctionClass
  {
  public:

    HoardManager()
      : _magic (MAGIC_NUMBER),
        _cachedSize (binType::getClassSize(0)),
        _cachedRealSize (_cachedSize),
        _cachedSizeClass (0),
        _allocationCount(0) // Initialize allocation count for tracking allocator demand
    {}

    virtual ~HoardManager() {}

    typedef SuperblockType_ SuperblockType;

    enum { Alignment = SuperblockType::Header::Alignment };

    bool isSystemLoadLow() {
      std::ifstream loadavgFile("/proc/loadavg");
      double loadavg;
      if (loadavgFile >> loadavg) {
        auto numProcessors = sysconf(_SC_NPROCESSORS_ONLN);
        printf ("Current load average: {%f}", loadavg);
        return loadavg < static_cast<double>(numProcessors);
      }
      printf ("Error: Could not read load average from /proc/loadavg");
      return false; // Default to false if unable to read load average
    }

    // Implementation for isAllocatorDemandHigh
    bool isAllocatorDemandHigh() {
        const unsigned int HIGH_DEMAND_THRESHOLD = 10000; // Example threshold
        printf ("Allocator Demand High");
        return _allocationCount > HIGH_DEMAND_THRESHOLD;
    }


    MALLOC_FUNCTION INLINE void * malloc (size_t sz)
    {
      Check<HoardManager, sanityCheck> check (this);
      int binIndex;
      size_t realSize;
      if (false) { // sz == _cachedSize) {
	binIndex = _cachedSizeClass;
	realSize = _cachedRealSize;
      } else {
	binIndex = binType::getSizeClass(sz);
	realSize = binType::getClassSize (binIndex);
	_cachedSize = sz;
	_cachedSizeClass = binIndex;
	_cachedRealSize = realSize;
      }
      assert (realSize >= sz);

      // Iterate until we succeed in allocating memory.
      auto ptr = getObject (binIndex, realSize);
      if (!ptr) {
	ptr = slowPathMalloc (realSize);
      }
      assert (SuperHeap::getSize(ptr) >= sz);
      assert ((size_t) ptr % Alignment == 0);
      return ptr;
    }


    // Modified put method
    NO_INLINE void put (SuperblockType * s, size_t sz) {
      std::lock_guard<LockType> l (_theLock);

      assert (s->getOwner() != this);
      Check<HoardManager, sanityCheck> check (this);

      const auto binIndex = binType::getSizeClass(sz);

      auto& stats = _stats(binIndex);
      auto a = stats.getAllocated() + s->getTotalObjects();
      auto u = stats.getInUse() + (s->getTotalObjects() - s->getObjectsFree());

      bool aggressiveReclaim = isSystemLoadLow() || isAllocatorDemandHigh();

      if (aggressiveReclaim ? thresholdFunctionClass::function(u, a, sz, true) : thresholdFunctionClass::function(u, a, sz, false)) {
        _ph.put(reinterpret_cast<typename ParentHeap::SuperblockType *>(s), sz);
      } else {
        unlocked_put(s, sz);
      }
    }


    // Modified get method
    NO_INLINE SuperblockType * get (size_t sz, HeapType * dest) {
      std::lock_guard<LockType> l (_theLock);
      Check<HoardManager, sanityCheck> check (this);
      const auto binIndex = binType::getSizeClass(sz);

      bool lessAggressiveFetch = isAllocatorDemandHigh();

      SuperblockType * s = nullptr;
      if (!lessAggressiveFetch) {
        s = _otherBins(binIndex).get();
      }

      if (s) {
        assert(s->isValidSuperblock());
      
        decStatsSuperblock(s, binIndex);
        s->setOwner(dest);
      }
      return s;
    }
    

    /// Return one object to its superblock and update stats.
    INLINE void free (void * ptr) {
      Check<HoardManager, sanityCheck> check (this);

      // Get the corresponding superblock.
      SuperblockType * s = SuperHeap::getSuperblock (ptr);
 
      assert (s->getOwner() == this);

      // Find out which bin it belongs to.
      // Note that we assume that all pointers have been correctly
      // normalized at this point.
      assert (s->normalize (ptr) == ptr);

      auto sz = s->getObjectSize ();
      auto binIndex = (int) binType::getSizeClass (sz);

      // Free the object.
      _otherBins(binIndex).free (ptr);


      // Update statistics.
      auto& stats = _stats(binIndex);
      auto u = stats.getInUse();
      auto a = stats.getAllocated();
      u--;
      stats.setInUse (u);

      // Free up a superblock if we've crossed the emptiness threshold.

      if (thresholdFunctionClass::function (u, a, sz, false)) {

	slowPathFree (binIndex, u, a);

      }
    }

    INLINE void lock() {
      _theLock.lock();
    }

    INLINE void unlock() {
      _theLock.unlock();
    }

  private:
    typedef BaseHoardManager<SuperblockType_> SuperHeap;

    enum { SuperblockSize = sizeof(SuperblockType_) };

    /// Ensure that the superblock size is a power of two.
    static_assert((SuperblockSize & (SuperblockSize-1)) == 0,
		  "Superblock size must be a power of two.");

    enum { MAGIC_NUMBER = 0xfeeddadd };

    /// A magic number used for debugging.
    const unsigned long _magic;

    size_t _cachedSize;
    size_t _cachedRealSize;
    int    _cachedSizeClass;
    unsigned long _allocationCount; // For tracking allocator demand
    
    inline int isValid() const {
      return (_magic == MAGIC_NUMBER);
    }

    static_assert(sizeof(typename SuperblockType::Header) % sizeof(double) == 0,
		  "Header size must be a multiple of the size of a double.");


    /// The type of the bin manager.
    typedef HL::bins<typename SuperblockType::Header, SuperblockSize> binType;

    /// How many bins do we need to maintain?
    enum { NumBins = binType::NUM_BINS };

    NO_INLINE void slowPathFree (int binIndex, unsigned int u, unsigned int a) {
      // We've crossed the threshold.
      // Remove a superblock and give it to the 'parent heap.'
      Check<HoardManager, sanityCheck> check (this);
    
      //	printf ("HoardManager: this = %x, getting a superblock\n", this);
    
      SuperblockType * sb = _otherBins(binIndex).get ();
    
      // We should always get one.
      assert (sb);
      if (sb) {

	auto sz = binType::getClassSize (binIndex);
	auto& stats = _stats(binIndex);
	auto totalObjects = sb->getTotalObjects();
	stats.setInUse (u - (totalObjects - sb->getObjectsFree()));
	stats.setAllocated (a - totalObjects);

	// Give it to the parent heap.
	///////// NOTE: We change the superblock type here!
	///////// THIS HAD BETTER BE SAFE!
	_ph.put (reinterpret_cast<typename ParentHeap::SuperblockType *>(sb), sz);
	assert (sb->isValidSuperblock());

      }
    }


    NO_INLINE void unlocked_put (SuperblockType * s, size_t sz) {
      if (!s || !s->isValidSuperblock()) {
	return;
      }

      Check<HoardManager, sanityCheck> check (this);

      const auto binIndex = binType::getSizeClass(sz);

      // Now put it on this heap.
      s->setOwner (reinterpret_cast<HeapType *>(this));
      _otherBins(binIndex).put (s);

      // Update the heap statistics with the allocated and in use stats
      // for the superblock.

      addStatsSuperblock (s, binIndex);
      assert (s->isValidSuperblock());

    }

    void addStatsSuperblock (SuperblockType * s, int binIndex) {
      auto& stats = _stats(binIndex);
      auto a = stats.getAllocated();
      auto u = stats.getInUse();
      auto totalObjects = s->getTotalObjects();
      stats.setInUse (u + (totalObjects - s->getObjectsFree()));
      stats.setAllocated (a + totalObjects);
    }


    void decStatsSuperblock (SuperblockType * s, int binIndex) {
      auto& stats = _stats(binIndex);
      auto a = stats.getAllocated();
      auto u = stats.getInUse();
      auto totalObjects = s->getTotalObjects();
      stats.setInUse (u - (totalObjects - s->getObjectsFree()));
      stats.setAllocated (a - totalObjects);
    }

    MALLOC_FUNCTION NO_INLINE void * slowPathMalloc (size_t sz) {
      auto binIndex = binType::getSizeClass (sz);
      auto realSize = binType::getClassSize (binIndex);
      assert (realSize >= sz);
      for (;;) {
	Check<HoardManager, sanityCheck> check1 (this);
	auto * ptr = getObject (binIndex, realSize);
	if (ptr) {
	  return ptr;
	} else {
	  Check<HoardManager, sanityCheck> check2 (this);
	  // Return null if we can't allocate another superblock.
	  if (!getAnotherSuperblock (realSize)) {
	    //	  fprintf (stderr, "HoardManager::malloc - no memory.\n");
	    return 0;
	  }
	}
      }
    }

    /// Get one object of a particular size.
    MALLOC_FUNCTION INLINE void * getObject (int binIndex,
					     size_t sz) {
      Check<HoardManager, sanityCheck> check (this);
      void * ptr = _otherBins(binIndex).malloc (sz);
      if (ptr) {
	// We got one. Update stats.
	auto u = _stats(binIndex).getInUse();
	_stats(binIndex).setInUse (u+1);
      }
      return ptr;
    }

    friend class sanityCheck;

    class sanityCheck {
    public:
      inline static void precondition (HoardManager * h) {
	checkInvariant(h);
      }
      inline static void postcondition (HoardManager * h) {
	checkInvariant(h);
      }
    private:
      inline static void checkInvariant (HoardManager * h) {
	(void) h;
	assert (h->isValid());
      }
    };

  private:

    NO_INLINE void * getAnotherSuperblock (size_t sz) {

      // NB: This function should be on the slow path.

      // Try the parent heap.
      // NOTE: We change the superblock type here!
      auto * sb = reinterpret_cast<SuperblockType *>(_ph.get (sz, reinterpret_cast<ParentHeap *>(this)));

      if (sb) {
	if (!sb->isValidSuperblock()) {
	  // As above - drop any invalid superblocks.
	  sb = nullptr;
	}

      } else {
	// Nothing - get memory from the source.
	void * ptr = _sourceHeap.malloc (SuperblockSize);
	if (!ptr) {
	  return 0;
	}
	sb = new (ptr) SuperblockType (sz);
      }

      // Put the superblock into its appropriate bin.
      if (sb) {
	unlocked_put (sb, sz);
      }
      return sb;
    }

    LockType _theLock;

    /// Usage statistics for each bin.
    Array<NumBins, Statistics> _stats;

    typedef SuperblockType * SuperblockTypePointer;

    typedef EmptyClass<SuperblockType, EmptinessClasses> OrganizedByEmptiness;

    typedef ManageOneSuperblock<OrganizedByEmptiness> BinManager;

    /// Bins that hold superblocks for each size class.
    Array<NumBins, BinManager> _otherBins;

    /// The parent heap.
    ParentHeap _ph;

    /// Where memory comes from.
    SourceHeap _sourceHeap;

  };

} // namespace Hoard

#endif
