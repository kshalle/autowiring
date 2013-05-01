#ifndef _OUTSTANDING_COUNT_TRACKER_H
#define _OUTSTANDING_COUNT_TRACKER_H

class CoreContext;

class OutstandingCountTracker
{
public:
  OutstandingCountTracker(std::shared_ptr<CoreContext> context);
  ~OutstandingCountTracker(void);

private:
  std::shared_ptr<CoreContext> m_context;
};

#endif