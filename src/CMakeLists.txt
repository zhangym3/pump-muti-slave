idf_component_register(SRCS "main.cpp" "protocol.cpp" "flow_job_controller.cpp"
  "hardware.cpp" "storage.cpp" "mesh_transport.cpp" "recovery_gatt.cpp" "control_gateway.cpp" "application.cpp" "recovery_crypto.cpp" "master_identity.cpp" "recovery_attempt_secret.cpp"
  INCLUDE_DIRS "../include" REQUIRES bt nvs_flash mbedtls esp_driver_gpio esp_driver_usb_serial_jtag esp_timer esp_hw_support esp_system)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
