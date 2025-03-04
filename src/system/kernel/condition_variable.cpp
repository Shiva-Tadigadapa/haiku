/*
 * Copyright 2007-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2019, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <condition_variable.h>

#include <new>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <kscheduler.h>
#include <ksignal.h>
#include <int.h>
#include <listeners.h>
#include <scheduling_analysis.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/atomic.h>


#define STATUS_ADDED	1
#define STATUS_WAITING	2


static const int kConditionVariableHashSize = 512;


struct ConditionVariableHashDefinition {
	typedef const void* KeyType;
	typedef	ConditionVariable ValueType;

	size_t HashKey(const void* key) const
		{ return (size_t)key; }
	size_t Hash(ConditionVariable* variable) const
		{ return (size_t)variable->fObject; }
	bool Compare(const void* key, ConditionVariable* variable) const
		{ return key == variable->fObject; }
	ConditionVariable*& GetLink(ConditionVariable* variable) const
		{ return variable->fNext; }
};

typedef BOpenHashTable<ConditionVariableHashDefinition> ConditionVariableHash;
static ConditionVariableHash sConditionVariableHash;
static rw_spinlock sConditionVariableHashLock;


static int
list_condition_variables(int argc, char** argv)
{
	ConditionVariable::ListAll();
	return 0;
}


static int
dump_condition_variable(int argc, char** argv)
{
	if (argc != 2) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	addr_t address = parse_expression(argv[1]);
	if (address == 0)
		return 0;

	ConditionVariable* variable = sConditionVariableHash.Lookup((void*)address);

	if (variable == NULL) {
		// It must be a direct pointer to a condition variable.
		variable = (ConditionVariable*)address;
	}

	if (variable != NULL) {
		variable->Dump();

		set_debug_variable("_cvar", (addr_t)variable);
		set_debug_variable("_object", (addr_t)variable->Object());

	} else
		kprintf("no condition variable at or with key %p\n", (void*)address);

	return 0;
}


// #pragma mark - ConditionVariableEntry


ConditionVariableEntry::ConditionVariableEntry()
	: fVariable(NULL)
{
}


ConditionVariableEntry::~ConditionVariableEntry()
{
	// We can use an "unsafe" non-atomic access of fVariable here, since we only
	// care whether it is non-NULL, not what its specific value is.
	if (fVariable != NULL)
		_RemoveFromVariable();
}


bool
ConditionVariableEntry::Add(const void* object)
{
	ASSERT(object != NULL);

	InterruptsLocker _;
	ReadSpinLocker hashLocker(sConditionVariableHashLock);

	ConditionVariable* variable = sConditionVariableHash.Lookup(object);

	if (variable == NULL) {
		fWaitStatus = B_ENTRY_NOT_FOUND;
		return false;
	}

	SpinLocker variableLocker(variable->fLock);
	hashLocker.Unlock();

	_AddToLockedVariable(variable);

	return true;
}


inline void
ConditionVariableEntry::_AddToLockedVariable(ConditionVariable* variable)
{
	ASSERT(fVariable == NULL);

	fThread = thread_get_current_thread();
	fVariable = variable;
	fWaitStatus = STATUS_ADDED;
	fVariable->fEntries.Add(this);
	atomic_add(&fVariable->fEntriesCount, 1);
}


void
ConditionVariableEntry::_RemoveFromVariable()
{
	// This section is critical because it can race with _NotifyLocked on the
	// variable's thread, so we must not be interrupted during it.
	InterruptsLocker _;

	ConditionVariable* variable = atomic_pointer_get(&fVariable);
	if (atomic_pointer_get_and_set(&fThread, (Thread*)NULL) == NULL) {
		// If fThread was already NULL, that means the variable is already
		// in the process of clearing us out (or already has finished doing so.)
		// We thus cannot access fVariable, and must spin until it is cleared.
		int32 tries = 0;
		while (atomic_pointer_get(&fVariable) != NULL) {
			tries++;
			if ((tries % 10000) == 0)
				panic("variable pointer was not unset for a long time!");
			cpu_pause();
		}

		return;
	}

	while (true) {
		if (atomic_pointer_get(&fVariable) == NULL) {
			// The variable must have cleared us out. Acknowledge this and return.
			atomic_add(&variable->fEntriesCount, -1);
			return;
		}

		// There is of course a small race between checking the pointer and then
		// the try_acquire in which the variable might clear out our fVariable.
		// However, in the case where we were the ones to clear fThread, the
		// variable will notice that and then wait for us to acknowledge the
		// removal by decrementing fEntriesCount, as we do above; and until
		// we do that, we may validly use our cached pointer to the variable.
		if (try_acquire_spinlock(&variable->fLock))
			break;
	}

	// We now hold the variable's lock. Remove ourselves.
	if (fVariable->fEntries.Contains(this))
		fVariable->fEntries.Remove(this);

	atomic_pointer_set(&fVariable, (ConditionVariable*)NULL);
	atomic_add(&variable->fEntriesCount, -1);
	release_spinlock(&variable->fLock);
}


status_t
ConditionVariableEntry::Wait(uint32 flags, bigtime_t timeout)
{
#if KDEBUG
	if (!are_interrupts_enabled()) {
		panic("ConditionVariableEntry::Wait() called with interrupts "
			"disabled, entry: %p, variable: %p", this, fVariable);
		return B_ERROR;
	}
#endif

	// The race in-between get_and_set and (re)set is irrelevant, because
	// if the status really is <= 0, we have already been or are about to
	// be removed from the variable, and nothing else is going to set the status.
	status_t waitStatus = atomic_get_and_set(&fWaitStatus, STATUS_WAITING);
	if (waitStatus <= 0) {
		fWaitStatus = waitStatus;
		return waitStatus;
	}

	InterruptsLocker _;

	thread_prepare_to_block(thread_get_current_thread(), flags,
		THREAD_BLOCK_TYPE_CONDITION_VARIABLE, atomic_pointer_get(&fVariable));

	waitStatus = atomic_get(&fWaitStatus);
	if (waitStatus <= 0) {
		// We were just woken up! Unblock ourselves immediately.
		thread_unblock(thread_get_current_thread(), waitStatus);
	}

	status_t error;
	if ((flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) != 0)
		error = thread_block_with_timeout(flags, timeout);
	else
		error = thread_block();

	_RemoveFromVariable();
	return error;
}


status_t
ConditionVariableEntry::Wait(const void* object, uint32 flags,
	bigtime_t timeout)
{
	if (Add(object))
		return Wait(flags, timeout);
	return B_ENTRY_NOT_FOUND;
}


// #pragma mark - ConditionVariable


/*!	Initialization method for anonymous (unpublished) condition variables.
*/
void
ConditionVariable::Init(const void* object, const char* objectType)
{
	fObject = object;
	fObjectType = objectType;
	new(&fEntries) EntryList;
	fEntriesCount = 0;
	B_INITIALIZE_SPINLOCK(&fLock);

	T_SCHEDULING_ANALYSIS(InitConditionVariable(this, object, objectType));
	NotifyWaitObjectListeners(&WaitObjectListener::ConditionVariableInitialized,
		this);
}


void
ConditionVariable::Publish(const void* object, const char* objectType)
{
	ASSERT(object != NULL);

	fObject = object;
	fObjectType = objectType;
	new(&fEntries) EntryList;
	fEntriesCount = 0;
	B_INITIALIZE_SPINLOCK(&fLock);

	T_SCHEDULING_ANALYSIS(InitConditionVariable(this, object, objectType));
	NotifyWaitObjectListeners(&WaitObjectListener::ConditionVariableInitialized,
		this);

	InterruptsWriteSpinLocker _(sConditionVariableHashLock);

	ASSERT_PRINT(sConditionVariableHash.Lookup(object) == NULL,
		"condition variable: %p\n", sConditionVariableHash.Lookup(object));

	sConditionVariableHash.InsertUnchecked(this);
}


void
ConditionVariable::Unpublish()
{
	ASSERT(fObject != NULL);

	InterruptsLocker _;
	WriteSpinLocker hashLocker(sConditionVariableHashLock);
	SpinLocker selfLocker(fLock);

#if KDEBUG
	ConditionVariable* variable = sConditionVariableHash.Lookup(fObject);
	if (variable != this) {
		panic("Condition variable %p not published, found: %p", this, variable);
		return;
	}
#endif

	sConditionVariableHash.RemoveUnchecked(this);
	fObject = NULL;
	fObjectType = NULL;

	hashLocker.Unlock();

	if (!fEntries.IsEmpty())
		_NotifyLocked(true, B_ENTRY_NOT_FOUND);
}


void
ConditionVariable::Add(ConditionVariableEntry* entry)
{
	InterruptsSpinLocker _(fLock);
	entry->_AddToLockedVariable(this);
}


status_t
ConditionVariable::Wait(uint32 flags, bigtime_t timeout)
{
	ConditionVariableEntry entry;
	Add(&entry);
	return entry.Wait(flags, timeout);
}


/*static*/ void
ConditionVariable::NotifyOne(const void* object, status_t result)
{
	InterruptsReadSpinLocker locker(sConditionVariableHashLock);
	ConditionVariable* variable = sConditionVariableHash.Lookup(object);
	locker.Unlock();
	if (variable == NULL)
		return;

	variable->NotifyOne(result);
}


/*static*/ void
ConditionVariable::NotifyAll(const void* object, status_t result)
{
	InterruptsReadSpinLocker locker(sConditionVariableHashLock);
	ConditionVariable* variable = sConditionVariableHash.Lookup(object);
	locker.Unlock();
	if (variable == NULL)
		return;

	variable->NotifyAll(result);
}


void
ConditionVariable::_Notify(bool all, status_t result)
{
	InterruptsSpinLocker _(fLock);

	if (!fEntries.IsEmpty()) {
		if (result > B_OK) {
			panic("tried to notify with invalid result %" B_PRId32 "\n", result);
			result = B_ERROR;
		}

		_NotifyLocked(all, result);
	}
}


/*! Called with interrupts disabled and the condition variable's spinlock held.
 */
void
ConditionVariable::_NotifyLocked(bool all, status_t result)
{
	// Dequeue and wake up the blocked threads.
	while (ConditionVariableEntry* entry = fEntries.RemoveHead()) {
		Thread* thread = atomic_pointer_get_and_set(&entry->fThread, (Thread*)NULL);
		if (thread == NULL) {
			// The entry must be in the process of trying to remove itself from us.
			// Clear its variable and wait for it to acknowledge this in fEntriesCount,
			// as it is the one responsible for decrementing that.
			const int32 oldCount = atomic_get(&fEntriesCount);
			atomic_pointer_set(&entry->fVariable, (ConditionVariable*)NULL);

			// As fEntriesCount is only modified while our lock is held, nothing else
			// will modify it while we are spinning, since we hold it at present.
			int32 tries = 0;
			while (atomic_get(&fEntriesCount) == oldCount) {
				tries++;
				if ((tries % 10000) == 0)
					panic("entries count was not decremented for a long time!");
				cpu_pause();
			}
		} else {
			status_t waitStatus = atomic_get_and_set(&entry->fWaitStatus, result);

			SpinLocker threadLocker(thread->scheduler_lock);
			if (waitStatus == STATUS_WAITING && thread->state != B_THREAD_WAITING) {
				// The thread is not in B_THREAD_WAITING state, so we must unblock it early,
				// in case it tries to re-block itself immediately after we unset fVariable.
				thread_unblock_locked(thread, result);
				waitStatus = result;
			}

			// No matter what the thread is doing, as we were the ones to clear its
			// fThread, so we are the ones responsible for decrementing fEntriesCount.
			// (We may not validly access the entry once we unset its fVariable.)
			atomic_pointer_set(&entry->fVariable, (ConditionVariable*)NULL);
			atomic_add(&fEntriesCount, -1);

			// If the thread was in B_THREAD_WAITING state, we unblock it after unsetting
			// fVariable, because otherwise it will wake up before thread_unblock returns
			// and spin while waiting for us to do so.
			if (waitStatus == STATUS_WAITING)
				thread_unblock_locked(thread, result);
		}

		if (!all)
			break;
	}
}


/*static*/ void
ConditionVariable::ListAll()
{
	kprintf("  variable      object (type)                waiting threads\n");
	kprintf("------------------------------------------------------------\n");
	ConditionVariableHash::Iterator it(&sConditionVariableHash);
	while (ConditionVariable* variable = it.Next()) {
		// count waiting threads
		int count = variable->fEntries.Count();

		kprintf("%p  %p  %-20s %15d\n", variable, variable->fObject,
			variable->fObjectType, count);
	}
}


void
ConditionVariable::Dump() const
{
	kprintf("condition variable %p\n", this);
	kprintf("  object:  %p (%s)\n", fObject, fObjectType);
	kprintf("  threads:");

	for (EntryList::ConstIterator it = fEntries.GetIterator();
		 ConditionVariableEntry* entry = it.Next();) {
		kprintf(" %" B_PRId32, entry->fThread->id);
	}
	kprintf("\n");
}


// #pragma mark -


void
condition_variable_init()
{
	new(&sConditionVariableHash) ConditionVariableHash;

	status_t error = sConditionVariableHash.Init(kConditionVariableHashSize);
	if (error != B_OK) {
		panic("condition_variable_init(): Failed to init hash table: %s",
			strerror(error));
	}

	add_debugger_command_etc("cvar", &dump_condition_variable,
		"Dump condition variable info",
		"<address>\n"
		"Prints info for the specified condition variable.\n"
		"  <address>  - Address of the condition variable or the object it is\n"
		"               associated with.\n", 0);
	add_debugger_command_etc("cvars", &list_condition_variables,
		"List condition variables",
		"\n"
		"Lists all existing condition variables\n", 0);
}
