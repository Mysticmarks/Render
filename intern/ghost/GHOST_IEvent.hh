/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_IEvent interface class.
 */

#pragma once

#include "GHOST_Types.h"
#include <cstddef>

class GHOST_IWindow;

/**
 * Interface class for events received from GHOST.
 * You should not need to inherit this class. The system will pass these events
 * to the #GHOST_IEventConsumer::processEvent() method of event consumers.<br>
 * Use the #getType() method to retrieve the type of event and the #getData()
 * method to get the event data out. Using the event type you can cast the
 * event data to the correct event data structure.
 * \see GHOST_IEventConsumer#processEvent
 * \see GHOST_TEventType
 */
class GHOST_IEvent {
 public:
  /**
   * Destructor.
   */
  virtual ~GHOST_IEvent() = default;

  /**
   * Returns the event type.
   * \return The event type.
   */
  virtual GHOST_TEventType getType() const = 0;

  /**
   * Returns the time this event was generated.
   * \return The event generation time.
   */
  virtual uint64_t getTime() const = 0;

  /**
   * Returns the window this event was generated on,
   * or nullptr if it is a 'system' event.
   * \return The generating window.
   */
  virtual GHOST_IWindow *getWindow() const = 0;

  /**
   * Returns the event data.
   * \return The event data.
   */
  virtual GHOST_TEventDataPtr getData() const = 0;

  MEM_CXX_CLASS_ALLOC_FUNCS("GHOST:GHOST_IEvent")
};
