extern "C" const char* __lsan_default_suppressions() {
    return "leak:dbus_message_new_empty_header\n"
           "leak:_dbus_message_loader_queue_messages\n"
           "leak:dbus_bus_register\n";
}
