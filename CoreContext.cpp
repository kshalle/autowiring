// Copyright (c) 2010 - 2013 Leap Motion. All rights reserved. Proprietary and confidential.
#include "stdafx.h"
#include "CoreContext.h"
#include "CoreThread.h"
#include "AutoPacketFactory.h"
#include "AutoPacketListener.h"
#include "Autowired.h"
#include "BoltBase.h"
#include "GlobalCoreContext.h"
#include "MicroBolt.h"
#include <algorithm>
#include <stack>
#include <boost/thread/tss.hpp>

using namespace std;

/// <summary>
/// A pointer to the current context, specific to the current thread.
/// </summary>
/// <remarks>
/// All threads have a current context, and this pointer refers to that current context.  If this value is null,
/// then the current context is the global context.  It's very important that threads not attempt to hold a reference
/// to the global context directly because it could change teardown order if the main thread sets the global context
/// as current.
/// </remarks>
boost::thread_specific_ptr<std::shared_ptr<CoreContext>> s_curContext;

CoreContext::CoreContext(std::shared_ptr<CoreContext> pParent) :
  m_pParent(pParent),
  m_initiated(false),
  m_isShutdown(false),
  m_junctionBoxManager(std::make_shared<JunctionBoxManager>())
{}

// Peer Context Constructor. Called interally by CreatePeer
CoreContext::CoreContext(std::shared_ptr<CoreContext> pParent, std::shared_ptr<CoreContext> pPeer) :
  m_pParent(pParent),
  m_initiated(false),
  m_isShutdown(false),
  m_junctionBoxManager(pPeer->m_junctionBoxManager)
{}

CoreContext::~CoreContext(void) {
  // The s_curContext pointer holds a shared_ptr to this--if we're in a dtor, and our caller
  // still holds a reference to us, then we have a serious problem.
  assert(
    !s_curContext.get() ||
    !s_curContext.get()->use_count() ||
    s_curContext.get()->get() != this
  );

  // Notify all ContextMember instances that their parent is going away
  NotifyTeardownListeners();

  // Make sure events aren't happening anymore:
  UnregisterEventReceivers();

  // Tell all context members that we're tearing down:
  for(auto q : m_contextMembers)
    q->NotifyContextTeardown();
}

std::shared_ptr<Object> CoreContext::IncrementOutstandingThreadCount(void) {
  std::shared_ptr<Object> retVal = m_outstanding.lock();
  if(retVal)
    return retVal;

  auto self = shared_from_this();

  // Increment the parent's outstanding count as well.  This will be held by the lambda, and will cause the enclosing
  // context's outstanding thread count to be incremented by one as long as we have any threads still running in our
  // context.  This property is relied upon in order to get the Wait function to operate properly.
  std::shared_ptr<Object> parentCount;
  if(m_pParent)
    parentCount = m_pParent->IncrementOutstandingThreadCount();

  retVal.reset(
    (Object*)1,
    [this, self, parentCount](Object*) {
      // Object being destroyed, notify all recipients
      boost::lock_guard<boost::mutex> lk(m_lock);

      // Unfortunately, this destructor callback is made before weak pointers are
      // invalidated, which requires that we manually reset the outstanding count
      m_outstanding.reset();

      // Wake everyone up
      m_stateChanged.notify_all();
    }
  );
  m_outstanding = retVal;
  return retVal;
}

void CoreContext::AddInternal(const AddInternalTraits& traits) {
  {
    boost::unique_lock<boost::mutex> lk(m_lock);

    // Validate that this addition does not generate an ambiguity:
    auto& v = m_typeMemos[typeid(*traits.pObject)];
    if(*v.m_value == traits.pObject)
      throw std::runtime_error("An attempt was made to add the same value to the same context more than once");
    if(*v.m_value)
      throw std::runtime_error("An attempt was made to add the same type to the same context more than once");

    // Add the new concrete type:
    m_concreteTypes.push_back(traits.value);

    // Insert each context element:
    if(traits.pContextMember)
      AddContextMember(traits.pContextMember);

    if(traits.pCoreRunnable)
      AddCoreRunnable(traits.pCoreRunnable);

    if(traits.pFilter)
      m_filters.push_back(traits.pFilter.get());

    if(traits.pBoltBase)
      AddBolt(traits.pBoltBase);

    // Notify any autowiring field that is currently waiting that we have a new member
    // to be considered.
    UpdateDeferredElements(std::move(lk), traits.pObject);
  }

  // Event receivers:
  if(traits.pRecvr) {
    JunctionBoxEntry<EventReceiver> entry(this, traits.pRecvr);

    // Add to our vector of local receivers first:
    (boost::lock_guard<boost::mutex>)m_lock,
    m_eventReceivers.insert(entry);

    // Recursively add to all junction box managers up the stack:
    AddEventReceiver(entry);
  }

  // Subscribers, if applicable:
  if(traits.subscriber)
    AddPacketSubscriber(traits.subscriber);

  // Signal listeners that a new object has been created
  GetGlobal()->Invoke(&AutowiringEvents::NewObject)(*this, *traits.pObject.get());
}

std::shared_ptr<CoreContext> CoreContext::GetGlobal(void) {
  return std::static_pointer_cast<CoreContext, GlobalCoreContext>(GlobalCoreContext::Get());
}

std::vector<std::shared_ptr<BasicThread>> CoreContext::CopyBasicThreadList(void) const {
  std::vector<std::shared_ptr<BasicThread>> retVal;

  // It's safe to enumerate this list from outside of a protective lock because a linked list
  // has stable iterators, we do not delete entries from the interior of this list, and we only
  // add entries to the end of the list.
  for(auto q = m_threads.begin(); q != m_threads.end(); q++){
    BasicThread* thread = dynamic_cast<BasicThread*>(*q);
    if (thread)
      retVal.push_back((*thread).GetSelf<BasicThread>());
  }
  return retVal;
}

void CoreContext::Initiate(void) {
  {
    boost::lock_guard<boost::mutex> lk(m_lock);
    if(m_initiated)
      // Already running
      return;

    // Short-return if our stop flag has already been set:
    if(m_isShutdown)
      return;
    
    // Update flag value
    m_initiated = true;
  }

  if(m_pParent)
    // Start parent threads first
    m_pParent->Initiate();

  // Now we can add the event receivers we haven't been able to add because the context
  // wasn't yet started:
  AddEventReceivers(m_delayedEventReceivers.begin(), m_delayedEventReceivers.end());
  m_delayedEventReceivers.clear();
  m_junctionBoxManager->Initiate();

  // Reacquire the lock to prevent m_threads from being modified while we sit on it
  auto outstanding = IncrementOutstandingThreadCount();
  boost::lock_guard<boost::mutex> lk(m_lock);
  
  // Signal our condition variable
  m_stateChanged.notify_all();
  
  for(auto q : m_threads)
    q->Start(outstanding);
}

void CoreContext::InitiateCoreThreads(void) {
  Initiate();
}

void CoreContext::SignalShutdown(bool wait, ShutdownMode shutdownMode) {
  // Wipe out the junction box manager, notify anyone waiting on the state condition:
  {
    boost::lock_guard<boost::mutex> lk(m_lock);
    UnregisterEventReceivers();
    m_isShutdown = true;
    m_stateChanged.notify_all();
  }

  {
    // Teardown interleave assurance--all of these contexts will generally be destroyed
    // at the exit of this block, due to the behavior of SignalTerminate, unless exterior
    // context references (IE, related to snooping) exist.
    //
    // This is done in order to provide a stable collection that may be traversed during
    // teardown outside of a lock.
    std::vector<std::shared_ptr<CoreContext>> childrenInterleave;

    {
      // Tear down all the children.
      boost::lock_guard<boost::mutex> lk(m_lock);

      // Fill strong lock series in order to ensure proper teardown interleave:
      childrenInterleave.reserve(m_children.size());
      for(auto q = m_children.begin(); q != m_children.end(); q++) {
        auto childContext = q->lock();

        // Technically, it *is* possible for this weak pointer to be expired, even though
        // we're holding the lock.  This may happen if the context itself is exiting even
        // as we are processing SignalTerminate.  In that case, the child context in
        // question is blocking in its dtor lambda, waiting patiently until we're done,
        // at which point it will modify the m_children collection.
        if(!childContext)
          continue;

        // Add to the interleave so we can SignalTerminate in a controlled way.
        childrenInterleave.push_back(q->lock());
      }
    }

    // Now that we have a locked-down, immutable series, begin termination signalling:
    for(size_t i = childrenInterleave.size(); i--; )
      childrenInterleave[i]->SignalShutdown(wait);
  }

  // Pass notice to all child threads:
  bool graceful = shutdownMode == ShutdownMode::Graceful;
  for(t_threadList::iterator q = m_threads.begin(); q != m_threads.end(); ++q)
    (*q)->Stop(graceful);

  // Signal our condition variable
  m_stateChanged.notify_all();

  // Short-return if required:
  if(!wait)
    return;

  // Wait for the treads to finish before returning.
  for (t_threadList::iterator it = m_threads.begin(); it != m_threads.end(); ++it)
    (**it).Wait();
}

bool CoreContext::DelayUntilInitiated(void) {
  boost::unique_lock<boost::mutex> lk(m_lock);
  m_stateChanged.wait(lk, [this] {return m_initiated || m_isShutdown;});
  return !m_isShutdown;
}

std::shared_ptr<CoreContext> CoreContext::CurrentContext(void) {
  if(!s_curContext.get())
    return std::static_pointer_cast<CoreContext, GlobalCoreContext>(GetGlobalContext());

  std::shared_ptr<CoreContext>* retVal = s_curContext.get();
  assert(retVal);
  assert(*retVal);
  return *retVal;
}

void CoreContext::AddCoreRunnable(const std::shared_ptr<CoreRunnable>& ptr) {
  // Insert into the linked list of threads first:
  m_threads.push_front(ptr.get());

  if(m_initiated)
    // We're already running, this means we're late to the party and need to start _now_.
    ptr->Start(IncrementOutstandingThreadCount());

  if(m_isShutdown)
    // We're really late to the party, it's already over.  Make sure the thread's stop
    // overrides are called and that it transitions to a stopped state.
    ptr->Stop(false);
}

void CoreContext::AddBolt(const std::shared_ptr<BoltBase>& pBase) {
  for(auto cur = pBase->GetContextSigils(); *cur; cur++){
    m_nameListeners[**cur].push_back(pBase.get());
  }

  if(!*pBase->GetContextSigils())
    m_nameListeners[typeid(void)].push_back(pBase.get());
}

void CoreContext::BuildCurrentState(void) {
  AutoGlobalContext glbl;
  glbl->Invoke(&AutowiringEvents::NewContext)(*this);
  
  std::unordered_set<Object*> allObjects;

  // ContextMembers and CoreRunnables
  for (auto member : m_contextMembers) {
    Object* obj = dynamic_cast<Object*>(member);
    if (obj && allObjects.find(obj)==allObjects.end()) {
      GetGlobal()->Invoke(&AutowiringEvents::NewObject)(*this, *obj);
      allObjects.insert(obj);
    }
  }

  //Exception Filters
  for (auto filter : m_filters) {
    Object* obj = dynamic_cast<Object*>(filter);
    if (obj && allObjects.find(obj)==allObjects.end()){
      GetGlobal()->Invoke(&AutowiringEvents::NewObject)(*this, *obj);
      allObjects.insert(obj);
    }
  }

  //Event Receivers
  for (auto receiver : m_eventReceivers) {
    Object* obj = dynamic_cast<Object*>(receiver.m_ptr.get());
    if (obj && allObjects.find(obj)==allObjects.end()) {
      GetGlobal()->Invoke(&AutowiringEvents::NewObject)(*this, *obj);
      allObjects.insert(obj);
    }
  }

  boost::lock_guard<boost::mutex> lk(m_lock);
  for(auto c : m_children) {
    auto cur = c.lock();
    if(!cur)
      continue;

    // Recurse into the child instance:
    cur->BuildCurrentState();
  }
}

void CoreContext::CancelAutowiringNotification(DeferrableAutowiring* pDeferrable) {
  boost::lock_guard<boost::mutex> lk(m_lock);
  auto q = m_typeMemos.find(pDeferrable->GetType());
  if(q == m_typeMemos.end())
    return;

  // Always finalize this entry:
  auto strategy = pDeferrable->GetStrategy();
  if(strategy)
    strategy->Finalize(pDeferrable);

  // Stores the immediate predecessor of the node we will linearly scan for in our
  // linked list.
  DeferrableAutowiring* prior = nullptr;

  // Now remove the entry from the list:
  // NOTE:  If a performance bottleneck is tracked to here, the solution is to use
  // a doubly-linked list.
  for(auto cur = q->second.pFirst; cur && cur != pDeferrable; prior = cur, cur = cur->GetFlink())
    if(!cur)
      // Ran off the end of the list, nothing we can do here
      return;

  if(prior)
    // Erase the entry by using link elision:
    prior->SetFlink(pDeferrable->GetFlink());

  if(pDeferrable == q->second.pFirst)
    // Current deferrable is at the head, update the flink:
    q->second.pFirst = pDeferrable->GetFlink();
}

void CoreContext::Dump(std::ostream& os) const {
  boost::lock_guard<boost::mutex> lk(m_lock);
  for(auto q = m_typeMemos.begin(); q != m_typeMemos.end(); q++) {
    os << q->first.name();
    const void* pObj = q->second.m_value->ptr();
    if(pObj)
      os << " 0x" << hex << pObj;
    os << endl;
  }

  for(auto q = m_threads.begin(); q != m_threads.end(); q++) {
    BasicThread* pThread = dynamic_cast<BasicThread*>(*q);
    if (!pThread) continue;
    
    const char* name = pThread->GetName();
    os << "Thread " << pThread << " " << (name ? name : "(no name)") << std::endl;
  }
}

void ShutdownCurrentContext(void) {
  CoreContext::CurrentContext()->SignalShutdown();
}

void CoreContext::UnregisterEventReceivers(void) {
  // Release all event receivers originating from this context:
  for(auto q : m_eventReceivers)
    m_junctionBoxManager->RemoveEventReceiver(q);

  // Notify our parent (if we have one) that our event receivers are going away:
  if(m_pParent) {
    m_pParent->RemoveEventReceivers(m_eventReceivers.begin(), m_eventReceivers.end());

    auto pf = FindByTypeUnsafe<AutoPacketFactory>();
    if(pf)
      m_pParent->RemovePacketSubscribers(pf->GetSubscriberVector());
  }

  // Wipe out all collections so we don't try to free these multiple times:
  m_eventReceivers.clear();
}

void CoreContext::BroadcastContextCreationNotice(const std::type_info& sigil) const {
  auto q = m_nameListeners.find(sigil);
  if(q != m_nameListeners.end()) {
    // Iterate through all listeners:
    const auto& list = q->second;
    for(auto q = list.begin(); q != list.end(); q++)
      (**q).ContextCreated();
  }

  // In the case of an anonymous sigil type, we do not notify the all-types
  // listeners a second time.
  if(sigil != typeid(void)) {
    q = m_nameListeners.find(typeid(void));
    if(q != m_nameListeners.end())
      for(auto cur : q->second)
        cur->ContextCreated();
  }

  // Notify the parent next:
  if(m_pParent)
    m_pParent->BroadcastContextCreationNotice(sigil);
}

void CoreContext::UpdateDeferredElements(boost::unique_lock<boost::mutex>&& lk, const std::shared_ptr<Object>& entry) {
  // Collection of satisfiable lists:
  std::vector<std::pair<const DeferrableUnsynchronizedStrategy*, DeferrableAutowiring*>> satisfiable;

  // Notify any autowired field whose autowiring was deferred
  std::stack<DeferrableAutowiring*> stk;
  for(auto& cur : m_typeMemos) {
    auto& value = cur.second;

    if(value.m_value)
      // This entry is already satisfied, no need to process it
      continue;

    // Determine whether the current candidate element satisfies the autowiring we are considering.
    // This is done internally via a dynamic cast on the interface type for which this polymorphic
    // base type was constructed.
    if(!value.m_value->try_assign(entry))
      continue;

    // Now we need to take on the responsibility of satisfying this deferral.  We will do this by
    // nullifying the flink, and by ensuring that the memo is satisfied at the point where we
    // release the lock.
    stk.push(value.pFirst);
    value.pFirst = nullptr;

    // Finish satisfying the remainder of the chain while we hold the lock:
    while(!stk.empty()) {
      auto top = stk.top();
      stk.pop();

      for(auto* pNext = top; pNext; pNext = pNext->GetFlink()) {
        pNext->SatisfyAutowiring(value.m_value->shared_ptr());

        // See if there's another chain we need to process:
        auto child = pNext->ReleaseDependentChain();
        if(child)
          stk.push(child);

        // Not everyone needs to be finalized.  The entities that don't require finalization
        // are identified by an empty strategy, and we just skip them.
        auto strategy = pNext->GetStrategy();
        if(strategy)
          satisfiable.push_back(
            std::make_pair(strategy, pNext)
          );
      }
    }
  }

  // Give children a chance to also update their deferred elements:
  for(auto q = m_children.begin(); q != m_children.end(); q++) {
    // Hold reference to prevent this iterator from becoming invalidated:
    auto ctxt = q->lock();
    if(!ctxt)
      continue;

    // Reverse lock before satisfying children:
    lk.unlock();
    ctxt->UpdateDeferredElements(
      boost::unique_lock<boost::mutex>(ctxt->m_lock),
      entry
    );
    lk.lock();
  }
  lk.unlock();

  // Run through everything else and finalize it all:
  for(auto cur : satisfiable)
    cur.first->Finalize(cur.second);
}

void CoreContext::AddEventReceiver(JunctionBoxEntry<EventReceiver> entry) {
  {
    boost::lock_guard<boost::mutex> lk(m_lock);
    if (!m_initiated) {
      // Delay adding receiver until context is initialized
      m_delayedEventReceivers.insert(entry);
      return;
    }
  }

  m_junctionBoxManager->AddEventReceiver(entry);

  // Delegate ascending resolution, where possible.  This ensures that the parent context links
  // this event receiver to compatible senders in the parent context itself.
  if(m_pParent)
    m_pParent->AddEventReceiver(entry);
}


template<class iter>
void CoreContext::AddEventReceivers(iter first, iter last) {
  // Must be initiated
  assert(m_initiated);
  
  for(auto q = first; q != last; q++)
    m_junctionBoxManager->AddEventReceiver(*q);
  
  // Delegate ascending resolution, where possible.  This ensures that the parent context links
  // this event receiver to compatible senders in the parent context itself.
  if(m_pParent)
    m_pParent->AddEventReceivers(first, last);
}

void CoreContext::RemoveEventReceivers(t_rcvrSet::const_iterator first, t_rcvrSet::const_iterator last) {
  for(auto q = first; q != last; q++)
    m_junctionBoxManager->RemoveEventReceiver(*q);

  // Detour to the parent collection (if necessary)
  if(m_pParent)
    m_pParent->RemoveEventReceivers(first, last);
}

void CoreContext::UnsnoopEvents(Object* snooper, const JunctionBoxEntry<EventReceiver>& receiver) {
  m_junctionBoxManager->RemoveEventReceiver(receiver);
  
  // Decide if we should unsnoop the parent
  bool shouldRemove = m_pParent &&
                      ((boost::lock_guard<boost::mutex>)m_pParent->m_lock, true) &&
                      m_pParent->m_eventReceivers.find(receiver) == m_pParent->m_eventReceivers.end() &&
                      m_pParent->m_snoopers.find(snooper) == m_pParent->m_snoopers.end();
  
  if (shouldRemove)
    m_pParent->UnsnoopEvents(snooper, receiver);
}

void CoreContext::FilterException(void) {
  bool handled = false;
  for(auto q = m_filters.begin(); q != m_filters.end(); q++)
    try {
      (*q)->Filter(
        [] {
          std::rethrow_exception(std::current_exception());
        }
      );
      handled = true;
    } catch(...) {
      // Do nothing
    }

  // Pass to parent if one exists:
  if(m_pParent) {
    try {
      // See if the parent wants to handle this exception:
      m_pParent->FilterException();

      // Parent handled it, we're good to go
      return;
    } catch(...) {
    }
  }

  // Rethrow if unhandled:
  if(!handled)
    std::rethrow_exception(std::current_exception());
}

void CoreContext::FilterFiringException(const JunctionBoxBase* pProxy, EventReceiver* pRecipient) {
  auto rethrower = [] () {
    std::rethrow_exception(std::current_exception());
  };

  // Filter in order:
  for(CoreContext* pCur = this; pCur; pCur = pCur->GetParentContext().get())
    for(auto q = pCur->m_filters.begin(); q != pCur->m_filters.end(); q++)
      try {
        (*q)->Filter(rethrower, pProxy, pRecipient);
      } catch(...) {
        // Do nothing, filter didn't want to filter this exception
      }
}

std::shared_ptr<AutoPacketFactory> CoreContext::GetPacketFactory(void) {
  std::shared_ptr<AutoPacketFactory> pf;
  FindByType(pf);
  if(!pf)
    pf = Inject<AutoPacketFactory>();
  return pf;
}

void CoreContext::AddContextMember(const std::shared_ptr<ContextMember>& ptr) {
  // Always add to the set of context members
  m_contextMembers.push_back(ptr.get());
}

void CoreContext::AddPacketSubscriber(const AutoPacketSubscriber& rhs) {
  GetPacketFactory()->AddSubscriber(rhs);
  if(m_pParent)
    m_pParent->AddPacketSubscriber(rhs);
}

void CoreContext::UnsnoopAutoPacket(const AddInternalTraits& traits) {
  GetPacketFactory()->RemoveSubscriber(traits.type);
  
  // Decide if we should unsnoop the parent
  bool shouldRemove = m_pParent &&
                      ((boost::lock_guard<boost::mutex>)m_pParent->m_lock, true) &&
                      m_pParent->m_snoopers.find(traits.pObject.get()) == m_pParent->m_snoopers.end();
  
  // Check if snooper is a member of the parent
  if (shouldRemove)
    m_pParent->UnsnoopAutoPacket(traits);
}

void CoreContext::RemovePacketSubscribers(const std::vector<AutoPacketSubscriber> &subscribers) {
  // Notify the parent that it will have to remove these subscribers as well
  if(m_pParent)
    m_pParent->RemovePacketSubscribers(subscribers);

  // Remove subscribers from our factory AFTER the parent eviction has taken place
  auto factory = FindByTypeUnsafe<AutoPacketFactory>();
  if(factory)
    factory->RemoveSubscribers(subscribers.begin(), subscribers.end());
}

std::ostream& operator<<(std::ostream& os, const CoreContext& rhs) {
  rhs.Dump(os);
  return os;
}

void CoreContext::DebugPrintCurrentExceptionInformation() {
  try {
    std::rethrow_exception(std::current_exception());
  } catch(std::exception& ex) {
    std::cerr << ex.what() << std::endl;
  } catch(...) {
    // Nothing can be done, we don't know what exception type this is.
  }
}

std::shared_ptr<CoreContext> CoreContext::SetCurrent(void) {
  std::shared_ptr<CoreContext> newCurrent = this->shared_from_this();

  if(!newCurrent)
    throw std::runtime_error("Attempted to make a CoreContext current from a CoreContext ctor");

  std::shared_ptr<CoreContext> retVal = CoreContext::CurrentContext();
  s_curContext.reset(new std::shared_ptr<CoreContext>(newCurrent));
  return retVal;
}

void CoreContext::EvictCurrent(void) {
  s_curContext.reset();
}
