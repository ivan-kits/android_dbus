/* -*- mode: C; c-file-style: "gnu" -*- */
/* dispatch.c  Message dispatcher
 *
 * Copyright (C) 2003  CodeFactory AB
 * Copyright (C) 2003  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 1.2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dispatch.h"
#include "connection.h"
#include "driver.h"
#include "services.h"
#include "utils.h"
#include "bus.h"
#include "signals.h"
#include "test.h"
#include <dbus/dbus-internals.h>
#include <string.h>

static dbus_bool_t
send_one_message (DBusConnection *connection,
                  BusContext     *context,
                  DBusConnection *sender,
                  DBusConnection *addressed_recipient,
                  DBusMessage    *message,
                  BusTransaction *transaction,
                  DBusError      *error)
{
  if (!bus_context_check_security_policy (context,
                                          sender,
                                          addressed_recipient,
                                          connection,
                                          message,
                                          NULL))
    return TRUE; /* silently don't send it */
  
  if (!bus_transaction_send (transaction,
                             connection,
                             message))
    {
      BUS_SET_OOM (error);
      return FALSE;
    }

  return TRUE;
}

dbus_bool_t
bus_dispatch_matches (BusTransaction *transaction,
                      DBusConnection *sender,
                      DBusConnection *addressed_recipient,
                      DBusMessage    *message,
                      DBusError      *error)
{
  DBusError tmp_error;
  BusConnections *connections;
  DBusList *recipients;
  BusMatchmaker *matchmaker;
  DBusList *link;
  BusContext *context;

  _DBUS_ASSERT_ERROR_IS_CLEAR (error);

  /* sender and recipient can both be NULL for the bus driver,
   * or for signals with no particular recipient
   */

  _dbus_assert (sender == NULL || bus_connection_is_active (sender));
  _dbus_assert (dbus_message_get_sender (message) != NULL);

  connections = bus_transaction_get_connections (transaction);
  
  dbus_error_init (&tmp_error);
  context = bus_transaction_get_context (transaction);
  matchmaker = bus_context_get_matchmaker (context);

  recipients = NULL;
  if (!bus_matchmaker_get_recipients (matchmaker,
                                      bus_context_get_connections (context),
                                      sender, addressed_recipient, message,
                                      &recipients))
    {
      BUS_SET_OOM (error);
      return FALSE;
    }

  link = _dbus_list_get_first_link (&recipients);
  while (link != NULL)
    {
      DBusConnection *dest;

      dest = link->data;

      if (!send_one_message (dest, context, sender, addressed_recipient,
                             message, transaction, &tmp_error))
        break;

      link = _dbus_list_get_next_link (&recipients, link);
    }

  _dbus_list_clear (&recipients);
  
  if (dbus_error_is_set (&tmp_error))
    {
      dbus_move_error (&tmp_error, error);
      return FALSE;
    }
  else
    return TRUE;
}

static DBusHandlerResult
bus_dispatch (DBusConnection *connection,
              DBusMessage    *message)
{
  const char *sender, *service_name;
  DBusError error;
  BusTransaction *transaction;
  BusContext *context;
  DBusHandlerResult result;
  DBusConnection *addressed_recipient;
  
  result = DBUS_HANDLER_RESULT_HANDLED;
  
  transaction = NULL;
  addressed_recipient = NULL;
  dbus_error_init (&error);
  
  context = bus_connection_get_context (connection);
  _dbus_assert (context != NULL);
  
  /* If we can't even allocate an OOM error, we just go to sleep
   * until we can.
   */
  while (!bus_connection_preallocate_oom_error (connection))
    _dbus_wait_for_memory ();
  
  /* Ref connection in case we disconnect it at some point in here */
  dbus_connection_ref (connection);
  
  service_name = dbus_message_get_destination (message);

#ifdef DBUS_ENABLE_VERBOSE_MODE
  {
    const char *interface_name, *member_name, *error_name;

    interface_name = dbus_message_get_interface (message);
    member_name = dbus_message_get_member (message);
    error_name = dbus_message_get_error_name (message);
    
    _dbus_verbose ("DISPATCH: %s %s %s to %s\n",
                   interface_name ? interface_name : "(no interface)",
                   member_name ? member_name : "(no member)",
                   error_name ? error_name : "(no error name)",
                   service_name ? service_name : "peer");
  }
#endif /* DBUS_ENABLE_VERBOSE_MODE */
  
  /* If service_name is NULL, if it's a signal we send it to all
   * connections with a match rule. If it's not a signal, it goes to
   * the bus daemon but doesn't go "on the bus"; e.g. a peer-to-peer
   * ping. Handle these immediately, especially disconnection
   * messages. There are no security policy checks on these.
   */
  if (service_name == NULL)
    {
      if (dbus_message_is_signal (message,
                                  DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
                                  "Disconnected"))
        {
          bus_connection_disconnected (connection);
          goto out;
        }

      if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_SIGNAL)
        {
          /* DBusConnection also handles some of these automatically, we leave
           * it to do so.
           */
          result = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
          goto out;
        }
    }
  
  /* Create our transaction */
  transaction = bus_transaction_new (context);
  if (transaction == NULL)
    {
      BUS_SET_OOM (&error);
      goto out;
    }
  
  /* Assign a sender to the message */
  if (bus_connection_is_active (connection))
    {
      sender = bus_connection_get_name (connection);
      _dbus_assert (sender != NULL);

      if (!dbus_message_set_sender (message, sender))
        {
          BUS_SET_OOM (&error);
          goto out;
        }

      /* We need to refetch the service name here, because
       * dbus_message_set_sender can cause the header to be
       * reallocated, and thus the service_name pointer will become
       * invalid.
       */
      service_name = dbus_message_get_destination (message);
    }
  
  if (service_name &&
      strcmp (service_name, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS) == 0) /* to bus driver */
    {
      if (!bus_context_check_security_policy (context,
                                              connection, NULL, NULL, message, &error))
        {
          _dbus_verbose ("Security policy rejected message\n");
          goto out;
        }

      _dbus_verbose ("Giving message to %s\n", DBUS_SERVICE_ORG_FREEDESKTOP_DBUS);
      if (!bus_driver_handle_message (connection, transaction, message, &error))
        goto out;
    }
  else if (!bus_connection_is_active (connection)) /* clients must talk to bus driver first */
    {
      _dbus_verbose ("Received message from non-registered client. Disconnecting.\n");
      dbus_connection_disconnect (connection);
      goto out;
    }
  else if (service_name != NULL) /* route to named service */
    {
      DBusString service_string;
      BusService *service;
      BusRegistry *registry;

      _dbus_assert (service_name != NULL);
      
      registry = bus_connection_get_registry (connection);
      
      _dbus_string_init_const (&service_string, service_name);
      service = bus_registry_lookup (registry, &service_string);

      if (service == NULL)
        {
          dbus_set_error (&error,
                          DBUS_ERROR_SERVICE_DOES_NOT_EXIST,
                          "Service \"%s\" does not exist",
                          service_name);
          goto out;
        }
      else
        {          
          addressed_recipient = bus_service_get_primary_owner (service);
          _dbus_assert (addressed_recipient != NULL);
          
          if (!bus_context_check_security_policy (context,
                                                  connection, addressed_recipient,
                                                  addressed_recipient,
                                                  message, &error))
            goto out;
          
          /* Dispatch the message */
          if (!bus_transaction_send (transaction, addressed_recipient, message))
            {
              BUS_SET_OOM (&error);
              goto out;
            }
        }
    }

  /* Now match the messages against any match rules, which will send
   * out signals and such. addressed_recipient may == NULL.
   */
  if (!bus_dispatch_matches (transaction, connection, addressed_recipient, message, &error))
    goto out;
  
 out:
  if (dbus_error_is_set (&error))
    {
      if (!dbus_connection_get_is_connected (connection))
        {
          /* If we disconnected it, we won't bother to send it any error
           * messages.
           */
          _dbus_verbose ("Not sending error to connection we disconnected\n");
        }
      else if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
        {
          bus_connection_send_oom_error (connection, message);

          /* cancel transaction due to OOM */
          if (transaction != NULL)
            {
              bus_transaction_cancel_and_free (transaction);
              transaction = NULL;
            }
        }
      else
        {
          /* Try to send the real error, if no mem to do that, send
           * the OOM error
           */
          _dbus_assert (transaction != NULL);
          
          if (!bus_transaction_send_error_reply (transaction, connection,
                                                 &error, message))
            {
              bus_connection_send_oom_error (connection, message);
              
              /* cancel transaction due to OOM */
              if (transaction != NULL)
                {
                  bus_transaction_cancel_and_free (transaction);
                  transaction = NULL;
                }
            }
        }
      
      dbus_error_free (&error);
    }

  if (transaction != NULL)
    {
      bus_transaction_execute_and_free (transaction);
    }

  dbus_connection_unref (connection);

  return result;
}

static DBusHandlerResult
bus_dispatch_message_filter (DBusConnection     *connection,
                             DBusMessage        *message,
                             void               *user_data)
{
  return bus_dispatch (connection, message);
}

dbus_bool_t
bus_dispatch_add_connection (DBusConnection *connection)
{  
  if (!dbus_connection_add_filter (connection,
                                   bus_dispatch_message_filter,
                                   NULL, NULL))
    return FALSE;
  
  return TRUE;
}

void
bus_dispatch_remove_connection (DBusConnection *connection)
{
  /* Here we tell the bus driver that we want to get off. */
  bus_driver_remove_connection (connection);

  dbus_connection_remove_filter (connection,
                                 bus_dispatch_message_filter,
                                 NULL);
}

#ifdef DBUS_BUILD_TESTS

typedef dbus_bool_t (* Check1Func) (BusContext     *context);
typedef dbus_bool_t (* Check2Func) (BusContext     *context,
                                    DBusConnection *connection);

static dbus_bool_t check_no_leftovers (BusContext *context);

static void
block_connection_until_message_from_bus (BusContext     *context,
                                         DBusConnection *connection)
{
  while (dbus_connection_get_dispatch_status (connection) ==
         DBUS_DISPATCH_COMPLETE &&
         dbus_connection_get_is_connected (connection))
    {
      bus_test_run_bus_loop (context, TRUE);
      bus_test_run_clients_loop (FALSE);
    }
}

/* compensate for fact that pop_message() can return #NULL due to OOM */
static DBusMessage*
pop_message_waiting_for_memory (DBusConnection *connection)
{
  while (dbus_connection_get_dispatch_status (connection) ==
         DBUS_DISPATCH_NEED_MEMORY)
    _dbus_wait_for_memory ();

  return dbus_connection_pop_message (connection);
}

static void
warn_unexpected_real (DBusConnection *connection,
                      DBusMessage    *message,
                      const char     *expected,
                      const char     *function,
                      int             line)
{
  _dbus_warn ("%s:%d received message interface \"%s\" member \"%s\" error name \"%s\" on %p, expecting %s\n",
              function, line,
              dbus_message_get_interface (message) ?
              dbus_message_get_interface (message) : "(unset)",
              dbus_message_get_member (message) ?
              dbus_message_get_member (message) : "(unset)",
              dbus_message_get_error_name (message) ?
              dbus_message_get_error_name (message) : "(unset)",
              connection,
              expected);
}

#define warn_unexpected(connection, message, expected) \
  warn_unexpected_real (connection, message, expected, _DBUS_FUNCTION_NAME, __LINE__)

static void
verbose_message_received (DBusConnection *connection,
                          DBusMessage    *message)
{
  _dbus_verbose ("Received message interface \"%s\" member \"%s\" error name \"%s\" on %p\n",
                 dbus_message_get_interface (message) ?
                 dbus_message_get_interface (message) : "(unset)",
                 dbus_message_get_member (message) ?
                 dbus_message_get_member (message) : "(unset)",
                 dbus_message_get_error_name (message) ?
                 dbus_message_get_error_name (message) : "(unset)",
                 connection);
}

typedef struct
{
  const char *expected_service_name;
  dbus_bool_t failed;
} CheckServiceDeletedData;

static dbus_bool_t
check_service_deleted_foreach (DBusConnection *connection,
                               void           *data)
{
  CheckServiceDeletedData *d = data;
  DBusMessage *message;
  DBusError error;
  char *service_name;

  dbus_error_init (&error);
  d->failed = TRUE;
  service_name = NULL;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a message on %p, expecting %s\n",
                  connection, "ServiceDeleted");
      goto out;
    }
  else if (!dbus_message_is_signal (message,
                                    DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                    "ServiceDeleted"))
    {
      warn_unexpected (connection, message, "ServiceDeleted");

      goto out;
    }
  else
    {
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &service_name,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              _dbus_verbose ("no memory to get service name arg\n");
            }
          else
            {
              _dbus_assert (dbus_error_is_set (&error));
              _dbus_warn ("Did not get the expected single string argument\n");
              goto out;
            }
        }
      else if (strcmp (service_name, d->expected_service_name) != 0)
        {
          _dbus_warn ("expected deletion of service %s, got deletion of %s\n",
                      d->expected_service_name,
                      service_name);
          goto out;
        }
    }

  d->failed = FALSE;
  
 out:
  dbus_free (service_name);
  dbus_error_free (&error);
  
  if (message)
    dbus_message_unref (message);

  return !d->failed;
}

static void
kill_client_connection (BusContext     *context,
                        DBusConnection *connection)
{
  char *base_service;
  const char *s;
  CheckServiceDeletedData csdd;

  _dbus_verbose ("killing connection %p\n", connection);
  
  s = dbus_bus_get_base_service (connection);
  _dbus_assert (s != NULL);

  while ((base_service = _dbus_strdup (s)) == NULL)
    _dbus_wait_for_memory ();

  dbus_connection_ref (connection);
  
  /* kick in the disconnect handler that unrefs the connection */
  dbus_connection_disconnect (connection);

  bus_test_run_everything (context);
  
  _dbus_assert (bus_test_client_listed (connection));
  
  /* Run disconnect handler in test.c */
  if (bus_connection_dispatch_one_message (connection))
    _dbus_assert_not_reached ("something received on connection being killed other than the disconnect");
  
  _dbus_assert (!dbus_connection_get_is_connected (connection));
  dbus_connection_unref (connection);
  connection = NULL;
  _dbus_assert (!bus_test_client_listed (connection));
  
  csdd.expected_service_name = base_service;
  csdd.failed = FALSE;

  bus_test_clients_foreach (check_service_deleted_foreach,
                            &csdd);

  dbus_free (base_service);
  
  if (csdd.failed)
    _dbus_assert_not_reached ("didn't get the expected ServiceDeleted messages");
  
  if (!check_no_leftovers (context))
    _dbus_assert_not_reached ("stuff left in message queues after disconnecting a client");
}

static void
kill_client_connection_unchecked (DBusConnection *connection)
{
  /* This kills the connection without expecting it to affect
   * the rest of the bus.
   */  
  _dbus_verbose ("Unchecked kill of connection %p\n", connection);

  dbus_connection_ref (connection);
  dbus_connection_disconnect (connection);
  /* dispatching disconnect handler will unref once */
  if (bus_connection_dispatch_one_message (connection))
    _dbus_assert_not_reached ("message other than disconnect dispatched after failure to register");

  _dbus_assert (!bus_test_client_listed (connection));
  dbus_connection_unref (connection);
}

typedef struct
{
  dbus_bool_t failed;
} CheckNoMessagesData;

static dbus_bool_t
check_no_messages_foreach (DBusConnection *connection,
                           void           *data)
{
  CheckNoMessagesData *d = data;
  DBusMessage *message;

  message = pop_message_waiting_for_memory (connection);
  if (message != NULL)
    {
      warn_unexpected (connection, message, "no messages");

      d->failed = TRUE;
    }

  if (message)
    dbus_message_unref (message);
  return !d->failed;
}

typedef struct
{
  DBusConnection *skip_connection;
  const char *expected_service_name;
  dbus_bool_t failed;
} CheckServiceCreatedData;

static dbus_bool_t
check_service_created_foreach (DBusConnection *connection,
                               void           *data)
{
  CheckServiceCreatedData *d = data;
  DBusMessage *message;
  DBusError error;
  char *service_name;

  if (connection == d->skip_connection)
    return TRUE;

  dbus_error_init (&error);
  d->failed = TRUE;
  service_name = NULL;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a message on %p, expecting %s\n",
                  connection, "ServiceCreated");
      goto out;
    }
  else if (!dbus_message_is_signal (message,
                                    DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                    "ServiceCreated"))
    {
      warn_unexpected (connection, message, "ServiceCreated");
      goto out;
    }
  else
    {
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &service_name,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              _dbus_verbose ("no memory to get service name arg\n");
            }
          else
            {
              _dbus_assert (dbus_error_is_set (&error));
              _dbus_warn ("Did not get the expected single string argument\n");
              goto out;
            }
        }
      else if (strcmp (service_name, d->expected_service_name) != 0)
        {
          _dbus_warn ("expected creation of service %s, got creation of %s\n",
                      d->expected_service_name,
                      service_name);
          goto out;
        }
    }

  d->failed = FALSE;
  
 out:
  dbus_free (service_name);
  dbus_error_free (&error);
  
  if (message)
    dbus_message_unref (message);

  return !d->failed;
}

static dbus_bool_t
check_no_leftovers (BusContext *context)
{
  CheckNoMessagesData nmd;

  nmd.failed = FALSE;
  bus_test_clients_foreach (check_no_messages_foreach,
                            &nmd);
  
  if (nmd.failed)
    return FALSE;
  else
    return TRUE;
}

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_hello_message (BusContext     *context,
                     DBusConnection *connection)
{
  DBusMessage *message;
  dbus_uint32_t serial;
  dbus_bool_t retval;
  DBusError error;
  char *name;
  char *acquired;

  retval = FALSE;
  dbus_error_init (&error);
  name = NULL;
  acquired = NULL;
  message = NULL;

  _dbus_verbose ("check_hello_message for %p\n", connection);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "Hello");

  if (message == NULL)
    return TRUE;

  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  /* send our message */
  bus_test_run_clients_loop (TRUE);

  dbus_connection_ref (connection); /* because we may get disconnected */
  block_connection_until_message_from_bus (context, connection);

  if (!dbus_connection_get_is_connected (connection))
    {
      _dbus_verbose ("connection was disconnected\n");
      
      dbus_connection_unref (connection);
      
      return TRUE;
    }

  dbus_connection_unref (connection);
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a reply to %s %d on %p\n",
                  "Hello", serial, connection);
      goto out;
    }

  verbose_message_received (connection, message);

  if (!dbus_message_has_sender (message, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS))
    {
      _dbus_warn ("Message has wrong sender %s\n",
                  dbus_message_get_sender (message) ?
                  dbus_message_get_sender (message) : "(none)");
      goto out;
    }
  
  if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (dbus_message_is_error (message,
                                 DBUS_ERROR_NO_MEMORY))
        {
          ; /* good, this is a valid response */
        }
      else
        {
          warn_unexpected (connection, message, "not this error");

          goto out;
        }
    }
  else
    {
      CheckServiceCreatedData scd;
      
      if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_METHOD_RETURN)
        {
          ; /* good, expected */
        }
      else
        {
          warn_unexpected (connection, message, "method return for Hello");

          goto out;
        }

    retry_get_hello_name:
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              _dbus_verbose ("no memory to get service name arg from hello\n");
              dbus_error_free (&error);
              _dbus_wait_for_memory ();
              goto retry_get_hello_name;
            }
          else
            {
              _dbus_assert (dbus_error_is_set (&error));
              _dbus_warn ("Did not get the expected single string argument to hello\n");
              goto out;
            }
        }

      _dbus_verbose ("Got hello name: %s\n", name);

      while (!dbus_bus_set_base_service (connection, name))
        _dbus_wait_for_memory ();
      
      scd.skip_connection = connection; /* we haven't done AddMatch so won't get it ourselves */
      scd.failed = FALSE;
      scd.expected_service_name = name;
      bus_test_clients_foreach (check_service_created_foreach,
                                &scd);
      
      if (scd.failed)
        goto out;
      
      /* Client should also have gotten ServiceAcquired */
      dbus_message_unref (message);
      message = pop_message_waiting_for_memory (connection);
      if (message == NULL)
        {
          _dbus_warn ("Expecting %s, got nothing\n",
                      "ServiceAcquired");
          goto out;
        }
      
    retry_get_acquired_name:
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &acquired,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              _dbus_verbose ("no memory to get service name arg from acquired\n");
              dbus_error_free (&error);
              _dbus_wait_for_memory ();
              goto retry_get_acquired_name;
            }
          else
            {
              _dbus_assert (dbus_error_is_set (&error));
              _dbus_warn ("Did not get the expected single string argument to ServiceAcquired\n");
              goto out;
            }
        }

      _dbus_verbose ("Got acquired name: %s\n", acquired);

      if (strcmp (acquired, name) != 0)
        {
          _dbus_warn ("Acquired name is %s but expected %s\n",
                      acquired, name);
          goto out;
        }
    }

  if (!check_no_leftovers (context))
    goto out;
  
  retval = TRUE;
  
 out:
  dbus_error_free (&error);
  
  dbus_free (name);
  dbus_free (acquired);
  
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_add_match_all (BusContext     *context,
                     DBusConnection *connection)
{
  DBusMessage *message;
  dbus_bool_t retval;
  dbus_uint32_t serial;
  DBusError error;

  retval = FALSE;
  dbus_error_init (&error);
  message = NULL;

  _dbus_verbose ("check_add_match_all for %p\n", connection);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "AddMatch");

  if (message == NULL)
    return TRUE;

  if (!dbus_message_append_args (message, DBUS_TYPE_STRING, "", /* FIXME */
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      return TRUE;
    }
  
  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  /* send our message */
  bus_test_run_clients_loop (TRUE);

  dbus_connection_ref (connection); /* because we may get disconnected */
  block_connection_until_message_from_bus (context, connection);

  if (!dbus_connection_get_is_connected (connection))
    {
      _dbus_verbose ("connection was disconnected\n");
      
      dbus_connection_unref (connection);
      
      return TRUE;
    }

  dbus_connection_unref (connection);
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a reply to %s %d on %p\n",
                  "AddMatch", serial, connection);
      goto out;
    }

  verbose_message_received (connection, message);

  if (!dbus_message_has_sender (message, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS))
    {
      _dbus_warn ("Message has wrong sender %s\n",
                  dbus_message_get_sender (message) ?
                  dbus_message_get_sender (message) : "(none)");
      goto out;
    }
  
  if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (dbus_message_is_error (message,
                                 DBUS_ERROR_NO_MEMORY))
        {
          ; /* good, this is a valid response */
        }
      else
        {
          warn_unexpected (connection, message, "not this error");

          goto out;
        }
    }
  else
    {
      if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_METHOD_RETURN)
        {
          ; /* good, expected */
          _dbus_assert (dbus_message_get_reply_serial (message) == serial);
        }
      else
        {
          warn_unexpected (connection, message, "method return for AddMatch");

          goto out;
        }
    }

  if (!check_no_leftovers (context))
    goto out;
  
  retval = TRUE;
  
 out:
  dbus_error_free (&error);
  
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_hello_connection (BusContext *context)
{
  DBusConnection *connection;
  DBusError error;

  dbus_error_init (&error);

  connection = dbus_connection_open ("debug-pipe:name=test-server", &error);
  if (connection == NULL)
    {
      _DBUS_ASSERT_ERROR_IS_SET (&error);
      dbus_error_free (&error);
      return TRUE;
    }

  if (!bus_setup_debug_client (connection))
    {
      dbus_connection_disconnect (connection);
      dbus_connection_unref (connection);
      return TRUE;
    }

  if (!check_hello_message (context, connection))
    return FALSE;
  
  if (dbus_bus_get_base_service (connection) == NULL)
    {
      /* We didn't successfully register, so we can't
       * do the usual kill_client_connection() checks
       */
      kill_client_connection_unchecked (connection);
    }
  else
    {
      if (!check_add_match_all (context, connection))
        return FALSE;
      
      kill_client_connection (context, connection);
    }

  return TRUE;
}

#define NONEXISTENT_SERVICE_NAME "test.this.service.does.not.exist.ewuoiurjdfxcvn"

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_nonexistent_service_activation (BusContext     *context,
                                      DBusConnection *connection)
{
  DBusMessage *message;
  dbus_uint32_t serial;
  dbus_bool_t retval;
  DBusError error;
  
  dbus_error_init (&error);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "ActivateService");

  if (message == NULL)
    return TRUE;

  if (!dbus_message_append_args (message,
                                 DBUS_TYPE_STRING, NONEXISTENT_SERVICE_NAME,
                                 DBUS_TYPE_UINT32, 0,
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      return TRUE;
    }
  
  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  bus_test_run_everything (context);
  block_connection_until_message_from_bus (context, connection);
  bus_test_run_everything (context);

  if (!dbus_connection_get_is_connected (connection))
    {
      _dbus_verbose ("connection was disconnected\n");
      return TRUE;
    }
  
  retval = FALSE;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a reply to %s %d on %p\n",
                  "ActivateService", serial, connection);
      goto out;
    }

  verbose_message_received (connection, message);

  if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (!dbus_message_has_sender (message, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS))
        {
          _dbus_warn ("Message has wrong sender %s\n",
                      dbus_message_get_sender (message) ?
                      dbus_message_get_sender (message) : "(none)");
          goto out;
        }
      
      if (dbus_message_is_error (message,
                                 DBUS_ERROR_NO_MEMORY))
        {
          ; /* good, this is a valid response */
        }
      else if (dbus_message_is_error (message,
                                      DBUS_ERROR_ACTIVATE_SERVICE_NOT_FOUND))
        {
          ; /* good, this is expected also */
        }
      else
        {
          warn_unexpected (connection, message, "not this error");
          goto out;
        }
    }
  else
    {
      _dbus_warn ("Did not expect to successfully activate %s\n",
                  NONEXISTENT_SERVICE_NAME);
      goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

static dbus_bool_t
check_base_service_activated (BusContext     *context,
                              DBusConnection *connection,
                              DBusMessage    *initial_message,
                              char          **base_service_p)
{
  DBusMessage *message;
  dbus_bool_t retval;
  DBusError error;
  char *base_service;
  
  base_service = NULL;
  retval = FALSE;
  
  dbus_error_init (&error);

  message = initial_message;
  dbus_message_ref (message);  

  if (dbus_message_is_signal (message,
                              DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                              "ServiceCreated"))
    {
      char *service_name;
      CheckServiceCreatedData scd;

    reget_service_name_arg:
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &service_name,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              dbus_error_free (&error);
              _dbus_wait_for_memory ();
              goto reget_service_name_arg;
            }
          else
            {
              _dbus_warn ("Message %s doesn't have a service name: %s\n",
                          "ServiceCreated",
                          error.message);
              dbus_error_free (&error);
              goto out;
            }
        }

      if (*service_name != ':')
        {
          _dbus_warn ("Expected base service activation, got \"%s\" instead\n",
                      service_name);
          goto out;
        }
              
      base_service = service_name;
      service_name = NULL;
      
      scd.skip_connection = connection;
      scd.failed = FALSE;
      scd.expected_service_name = base_service;
      bus_test_clients_foreach (check_service_created_foreach,
                                &scd);
      
      if (scd.failed)
        goto out;
    }
  else
    {
      warn_unexpected (connection, message, "ServiceCreated for base service");

      goto out;
    }

  retval = TRUE;

  if (base_service_p)
    {
      *base_service_p = base_service;
      base_service = NULL;
    }
  
 out:
  if (message)
    dbus_message_unref (message);

  if (base_service)
    dbus_free (base_service);
  
  return retval;
}

static dbus_bool_t
check_service_activated (BusContext     *context,
                         DBusConnection *connection,
                         const char     *activated_name,
                         const char     *base_service_name,
                         DBusMessage    *initial_message)
{
  DBusMessage *message;
  dbus_bool_t retval;
  DBusError error;
  dbus_uint32_t activation_result;
  
  retval = FALSE;
  
  dbus_error_init (&error);

  message = initial_message;
  dbus_message_ref (message);

  if (dbus_message_is_signal (message,
                              DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                              "ServiceCreated"))
    {
      char *service_name;
      CheckServiceCreatedData scd;

    reget_service_name_arg:
      if (!dbus_message_get_args (message, &error,
                                  DBUS_TYPE_STRING, &service_name,
                                  DBUS_TYPE_INVALID))
        {
          if (dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
            {
              dbus_error_free (&error);
              _dbus_wait_for_memory ();
              goto reget_service_name_arg;
            }
          else
            {
              _dbus_warn ("Message %s doesn't have a service name: %s\n",
                          "ServiceCreated",
                          error.message);
              dbus_error_free (&error);
              goto out;
            }
        }

      if (strcmp (service_name, activated_name) != 0)
        {
          _dbus_warn ("Expected to see service %s created, saw %s instead\n",
                      activated_name, service_name);
          dbus_free (service_name);
          goto out;
        }
      
      scd.skip_connection = connection;
      scd.failed = FALSE;
      scd.expected_service_name = service_name;
      bus_test_clients_foreach (check_service_created_foreach,
                                &scd);
          
      dbus_free (service_name);

      if (scd.failed)
        goto out;
          
      dbus_message_unref (message);
      message = pop_message_waiting_for_memory (connection);
      if (message == NULL)
        {
          _dbus_warn ("Expected a reply to %s, got nothing\n",
                      "ActivateService");
          goto out;
        }
    }
  else
    {
      warn_unexpected (connection, message, "ServiceCreated for the activated name");
      
      goto out;
    }
  
  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
    {
      warn_unexpected (connection, message, "reply to ActivateService");

      goto out;
    }

  activation_result = 0;
  if (!dbus_message_get_args (message, &error,
                              DBUS_TYPE_UINT32, &activation_result,
                              DBUS_TYPE_INVALID))
    {
      if (!dbus_error_has_name (&error, DBUS_ERROR_NO_MEMORY))
        {
          _dbus_warn ("Did not have activation result first argument to %s: %s\n",
                      "ActivateService", error.message);
          dbus_error_free (&error);
          goto out;
        }

      dbus_error_free (&error);
    }
  else
    {
      if (activation_result == DBUS_ACTIVATION_REPLY_ACTIVATED)
        ; /* Good */
      else if (activation_result == DBUS_ACTIVATION_REPLY_ALREADY_ACTIVE)
        ; /* Good also */
      else
        {
          _dbus_warn ("Activation result was 0x%x, no good.\n",
                      activation_result);
          goto out;
        }
    }

  dbus_message_unref (message);
  message = NULL;
      
  if (!check_no_leftovers (context))
    {
      _dbus_warn ("Messages were left over after verifying existent activation results\n");
      goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

static dbus_bool_t
check_service_deactivated (BusContext     *context,
                           DBusConnection *connection,
                           const char     *activated_name,
                           const char     *base_service)
{
  DBusMessage *message;
  dbus_bool_t retval;
  DBusError error;
  CheckServiceDeletedData csdd;

  message = NULL;
  retval = FALSE;
  
  dbus_error_init (&error);

  /* Now we are expecting ServiceDeleted messages for the base
   * service and the activated_name.  The base service
   * notification is required to come last.
   */
  csdd.expected_service_name = activated_name;
  csdd.failed = FALSE;
  bus_test_clients_foreach (check_service_deleted_foreach,
                            &csdd);      

  if (csdd.failed)
    goto out;
      
  csdd.expected_service_name = base_service;
  csdd.failed = FALSE;
  bus_test_clients_foreach (check_service_deleted_foreach,
                            &csdd);

  if (csdd.failed)
    goto out;
      
  if (!check_no_leftovers (context))
    {
      _dbus_warn ("Messages were left over after verifying results of service exiting\n");
      goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

static dbus_bool_t
check_send_exit_to_service (BusContext     *context,
                            DBusConnection *connection,
                            const char     *service_name,
                            const char     *base_service)
{
  dbus_bool_t got_error;
  DBusMessage *message;
  dbus_uint32_t serial;
  dbus_bool_t retval;
  
  _dbus_verbose ("Sending exit message to the test service\n");

  retval = FALSE;
  
  /* Kill off the test service by sending it a quit message */
  message = dbus_message_new_method_call (service_name,
                                          "/org/freedesktop/TestSuite",
                                          "org.freedesktop.TestSuite",
                                          "Exit");
      
  if (message == NULL)
    {
      /* Do this again; we still need the service to exit... */
      if (!check_send_exit_to_service (context, connection,
                                       service_name, base_service))
        goto out;
      
      return TRUE;
    }
      
  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);

      /* Do this again; we still need the service to exit... */
      if (!check_send_exit_to_service (context, connection,
                                       service_name, base_service))
        goto out;
      
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  /* send message */
  bus_test_run_clients_loop (TRUE);

  /* read it in and write it out to test service */
  bus_test_run_bus_loop (context, FALSE);

  /* see if we got an error during message bus dispatching */
  bus_test_run_clients_loop (FALSE);
  message = dbus_connection_borrow_message (connection);
  got_error = message != NULL && dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR;
  if (message)
    {
      dbus_connection_return_message (connection, message);
      message = NULL;
    }
          
  if (!got_error)
    {
      /* If no error, wait for the test service to exit */
      block_connection_until_message_from_bus (context, connection);
              
      bus_test_run_everything (context);
    }

  if (got_error)
    {
      message = pop_message_waiting_for_memory (connection);
      _dbus_assert (message != NULL);

      if (!dbus_message_is_error (message,
                                  DBUS_ERROR_NO_MEMORY))
        {
          warn_unexpected (connection, message,
                           "a no memory error from asking test service to exit");
          goto out;
        }

      _dbus_verbose ("Got error %s when asking test service to exit\n",
                     dbus_message_get_error_name (message));

      /* Do this again; we still need the service to exit... */
      if (!check_send_exit_to_service (context, connection,
                                       service_name, base_service))
        goto out;
    }
  else
    {
      if (!check_service_deactivated (context, connection,
                                      service_name, base_service))
        goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

static dbus_bool_t
check_got_error (BusContext     *context,
                 DBusConnection *connection,
                 const char     *first_error_name,
                 ...)
{
  DBusMessage *message;
  dbus_bool_t retval;
  va_list ap;
  dbus_bool_t error_found;
  const char *error_name;
  
  retval = FALSE;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not get an expected error\n");
      goto out;
    }

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_ERROR)
    {
      warn_unexpected (connection, message, "an error");

      goto out;
    }

  error_found = FALSE;

  va_start (ap, first_error_name);
  error_name = first_error_name;
  while (error_name != NULL)
    {
      if (dbus_message_is_error (message, error_name))
        {
          error_found = TRUE;
          break;
        }
      error_name = va_arg (ap, char*);
    }
  va_end (ap);

  if (!error_found)
    {
      _dbus_warn ("Expected error %s or other, got %s instead\n",
                  first_error_name,
                  dbus_message_get_error_name (message));
      goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}
          
#define EXISTENT_SERVICE_NAME "org.freedesktop.DBus.TestSuiteEchoService"

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_existent_service_activation (BusContext     *context,
                                   DBusConnection *connection)
{
  DBusMessage *message;
  dbus_uint32_t serial;
  dbus_bool_t retval;
  DBusError error;
  char *base_service;

  base_service = NULL;
  
  dbus_error_init (&error);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "ActivateService");

  if (message == NULL)
    return TRUE;

  if (!dbus_message_append_args (message,
                                 DBUS_TYPE_STRING, EXISTENT_SERVICE_NAME,
                                 DBUS_TYPE_UINT32, 0,
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      return TRUE;
    }
  
  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  bus_test_run_everything (context);

  /* now wait for the message bus to hear back from the activated
   * service.
   */
  block_connection_until_message_from_bus (context, connection);

  bus_test_run_everything (context);

  if (!dbus_connection_get_is_connected (connection))
    {
      _dbus_verbose ("connection was disconnected\n");
      return TRUE;
    }
  
  retval = FALSE;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive any messages after %s %d on %p\n",
                  "ActivateService", serial, connection);
      goto out;
    }

  verbose_message_received (connection, message);
  _dbus_verbose ("  (after sending %s)\n", "ActivateService");

  if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (!dbus_message_has_sender (message, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS))
        {
          _dbus_warn ("Message has wrong sender %s\n",
                      dbus_message_get_sender (message) ?
                      dbus_message_get_sender (message) : "(none)");
          goto out;
        }
      
      if (dbus_message_is_error (message,
                                 DBUS_ERROR_NO_MEMORY))
        {
          ; /* good, this is a valid response */
        }
      else if (dbus_message_is_error (message,
                                      DBUS_ERROR_SPAWN_CHILD_EXITED))
        {
          ; /* good, this is expected also */
        }
      else
        {
          _dbus_warn ("Did not expect error %s\n",
                      dbus_message_get_error_name (message));
          goto out;
        }
    }
  else
    {
      dbus_bool_t got_service_deleted;
      dbus_bool_t got_error;
      
      if (!check_base_service_activated (context, connection,
                                         message, &base_service))
        goto out;

      dbus_message_unref (message);
      message = NULL;

      /* We may need to block here for the test service to exit or finish up */
      block_connection_until_message_from_bus (context, connection);
      
      message = dbus_connection_borrow_message (connection);
      if (message == NULL)
        {
          _dbus_warn ("Did not receive any messages after base service creation notification\n");
          goto out;
        }

      got_service_deleted = dbus_message_is_signal (message,
                                                    DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                                    "ServiceDeleted");
      got_error = dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR;
      
      dbus_connection_return_message (connection, message);
      message = NULL;

      if (got_error)
        {
          if (!check_got_error (context, connection,
                                DBUS_ERROR_SPAWN_CHILD_EXITED,
                                DBUS_ERROR_NO_MEMORY,
                                NULL))
            goto out;

          /* A service deleted should be coming along now after this error.
           * We can also get the error *after* the service deleted.
           */
          got_service_deleted = TRUE;
        }
      
      if (got_service_deleted)
        {
          /* The service started up and got a base address, but then
           * failed to register under EXISTENT_SERVICE_NAME
           */
          CheckServiceDeletedData csdd;
          
          csdd.expected_service_name = base_service;
          csdd.failed = FALSE;
          bus_test_clients_foreach (check_service_deleted_foreach,
                                    &csdd);

          if (csdd.failed)
            goto out;

          /* Now we should get an error about the service exiting
           * if we didn't get it before.
           */
          if (!got_error)
            {
              block_connection_until_message_from_bus (context, connection);
              
              /* and process everything again */
              bus_test_run_everything (context);
              
              if (!check_got_error (context, connection,
                                    DBUS_ERROR_SPAWN_CHILD_EXITED,
                                    NULL))
                goto out;
            }
        }
      else
        {
          message = pop_message_waiting_for_memory (connection);
          if (message == NULL)
            {
              _dbus_warn ("Failed to pop message we just put back! should have been a ServiceCreated\n");
              goto out;
            }
          
          if (!check_service_activated (context, connection, EXISTENT_SERVICE_NAME,
                                        base_service, message))
            goto out;
          
          dbus_message_unref (message);
          message = NULL;


          if (!check_no_leftovers (context))
            {
              _dbus_warn ("Messages were left over after successful activation\n");
              goto out;
            }

          if (!check_send_exit_to_service (context, connection,
                                           EXISTENT_SERVICE_NAME, base_service))
            goto out;
        }
    }
  
  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);

  if (base_service)
    dbus_free (base_service);
  
  return retval;
}

/* returns TRUE if the correct thing happens,
 * but the correct thing may include OOM errors.
 */
static dbus_bool_t
check_segfault_service_activation (BusContext     *context,
                                   DBusConnection *connection)
{
  DBusMessage *message;
  dbus_uint32_t serial;
  dbus_bool_t retval;
  DBusError error;
  
  dbus_error_init (&error);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "ActivateService");

  if (message == NULL)
    return TRUE;

  if (!dbus_message_append_args (message,
                                 DBUS_TYPE_STRING,
                                 "org.freedesktop.DBus.TestSuiteSegfaultService",
                                 DBUS_TYPE_UINT32, 0,
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      return TRUE;
    }
  
  if (!dbus_connection_send (connection, message, &serial))
    {
      dbus_message_unref (message);
      return TRUE;
    }

  dbus_message_unref (message);
  message = NULL;

  bus_test_run_everything (context);
  block_connection_until_message_from_bus (context, connection);
  bus_test_run_everything (context);

  if (!dbus_connection_get_is_connected (connection))
    {
      _dbus_verbose ("connection was disconnected\n");
      return TRUE;
    }
  
  retval = FALSE;
  
  message = pop_message_waiting_for_memory (connection);
  if (message == NULL)
    {
      _dbus_warn ("Did not receive a reply to %s %d on %p\n",
                  "ActivateService", serial, connection);
      goto out;
    }

  verbose_message_received (connection, message);

  if (dbus_message_get_type (message) == DBUS_MESSAGE_TYPE_ERROR)
    {
      if (!dbus_message_has_sender (message, DBUS_SERVICE_ORG_FREEDESKTOP_DBUS))
        {
          _dbus_warn ("Message has wrong sender %s\n",
                      dbus_message_get_sender (message) ?
                      dbus_message_get_sender (message) : "(none)");
          goto out;
        }
      
      if (dbus_message_is_error (message,
                                 DBUS_ERROR_NO_MEMORY))
        {
          ; /* good, this is a valid response */
        }
      else if (dbus_message_is_error (message,
                                      DBUS_ERROR_SPAWN_CHILD_SIGNALED))
        {
          ; /* good, this is expected also */
        }
      else
        {
          warn_unexpected (connection, message, "not this error");

          goto out;
        }
    }
  else
    {
      _dbus_warn ("Did not expect to successfully activate segfault service\n");
      goto out;
    }

  retval = TRUE;
  
 out:
  if (message)
    dbus_message_unref (message);
  
  return retval;
}

typedef struct
{
  Check1Func func;
  BusContext *context;
} Check1Data;

static dbus_bool_t
check_oom_check1_func (void *data)
{
  Check1Data *d = data;

  if (! (* d->func) (d->context))
    return FALSE;
  
  if (!check_no_leftovers (d->context))
    {
      _dbus_warn ("Messages were left over, should be covered by test suite\n");
      return FALSE;
    }

  return TRUE;
}

static void
check1_try_iterations (BusContext *context,
                       const char *description,
                       Check1Func  func)
{
  Check1Data d;

  d.func = func;
  d.context = context;

  if (!_dbus_test_oom_handling (description, check_oom_check1_func,
                                &d))
    _dbus_assert_not_reached ("test failed");
}

typedef struct
{
  Check2Func func;
  BusContext *context;
  DBusConnection *connection;
} Check2Data;

static dbus_bool_t
check_oom_check2_func (void *data)
{
  Check2Data *d = data;

  if (! (* d->func) (d->context, d->connection))
    return FALSE;
  
  if (!check_no_leftovers (d->context))
    {
      _dbus_warn ("Messages were left over, should be covered by test suite");
      return FALSE;
    }

  return TRUE;
}

static void
check2_try_iterations (BusContext     *context,
                       DBusConnection *connection,
                       const char     *description,
                       Check2Func      func)
{
  Check2Data d;

  d.func = func;
  d.context = context;
  d.connection = connection;
  
  if (!_dbus_test_oom_handling (description, check_oom_check2_func,
                                &d))
    _dbus_assert_not_reached ("test failed");
}

dbus_bool_t
bus_dispatch_test (const DBusString *test_data_dir)
{
  BusContext *context;
  DBusConnection *foo;
  DBusConnection *bar;
  DBusConnection *baz;
  DBusError error;

  dbus_error_init (&error);
  
  context = bus_context_new_test (test_data_dir,
                                  "valid-config-files/debug-allow-all.conf");
  if (context == NULL)
    return FALSE;
  
  foo = dbus_connection_open ("debug-pipe:name=test-server", &error);
  if (foo == NULL)
    _dbus_assert_not_reached ("could not alloc connection");

  if (!bus_setup_debug_client (foo))
    _dbus_assert_not_reached ("could not set up connection");

  if (!check_hello_message (context, foo))
    _dbus_assert_not_reached ("hello message failed");

  if (!check_add_match_all (context, foo))
    _dbus_assert_not_reached ("AddMatch message failed");
  
  bar = dbus_connection_open ("debug-pipe:name=test-server", &error);
  if (bar == NULL)
    _dbus_assert_not_reached ("could not alloc connection");

  if (!bus_setup_debug_client (bar))
    _dbus_assert_not_reached ("could not set up connection");

  if (!check_hello_message (context, bar))
    _dbus_assert_not_reached ("hello message failed");

  if (!check_add_match_all (context, bar))
    _dbus_assert_not_reached ("AddMatch message failed");
  
  baz = dbus_connection_open ("debug-pipe:name=test-server", &error);
  if (baz == NULL)
    _dbus_assert_not_reached ("could not alloc connection");

  if (!bus_setup_debug_client (baz))
    _dbus_assert_not_reached ("could not set up connection");

  if (!check_hello_message (context, baz))
    _dbus_assert_not_reached ("hello message failed");

  if (!check_add_match_all (context, baz))
    _dbus_assert_not_reached ("AddMatch message failed");
  
  if (!check_no_leftovers (context))
    {
      _dbus_warn ("Messages were left over after setting up initial connections");
      _dbus_assert_not_reached ("initial connection setup failed");
    }
  
  check1_try_iterations (context, "create_and_hello",
                         check_hello_connection);
  
  check2_try_iterations (context, foo, "nonexistent_service_activation",
                         check_nonexistent_service_activation);

  check2_try_iterations (context, foo, "segfault_service_activation",
                         check_segfault_service_activation);
  
  check2_try_iterations (context, foo, "existent_service_activation",
                         check_existent_service_activation);
  
  _dbus_verbose ("Disconnecting foo, bar, and baz\n");

  kill_client_connection_unchecked (foo);
  kill_client_connection_unchecked (bar);
  kill_client_connection_unchecked (baz);

  bus_context_unref (context);
  
  return TRUE;
}

dbus_bool_t
bus_dispatch_sha1_test (const DBusString *test_data_dir)
{
  BusContext *context;
  DBusConnection *foo;
  DBusError error;

  dbus_error_init (&error);
  
  /* Test SHA1 authentication */
  _dbus_verbose ("Testing SHA1 context\n");
  
  context = bus_context_new_test (test_data_dir,
                                  "valid-config-files/debug-allow-all-sha1.conf");
  if (context == NULL)
    return FALSE;

  foo = dbus_connection_open ("debug-pipe:name=test-server", &error);
  if (foo == NULL)
    _dbus_assert_not_reached ("could not alloc connection");

  if (!bus_setup_debug_client (foo))
    _dbus_assert_not_reached ("could not set up connection");

  if (!check_hello_message (context, foo))
    _dbus_assert_not_reached ("hello message failed");

  if (!check_add_match_all (context, foo))
    _dbus_assert_not_reached ("addmatch message failed");
  
  if (!check_no_leftovers (context))
    {
      _dbus_warn ("Messages were left over after setting up initial SHA-1 connection\n");
      _dbus_assert_not_reached ("initial connection setup failed");
    }
  
  check1_try_iterations (context, "create_and_hello_sha1",
                         check_hello_connection);

  kill_client_connection_unchecked (foo);

  bus_context_unref (context);

  return TRUE;
}

#endif /* DBUS_BUILD_TESTS */
