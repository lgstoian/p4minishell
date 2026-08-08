set(p4_usb_impl_dir "${CMAKE_CURRENT_LIST_DIR}/../usb")

idf_component_register(
    SRCS "${p4_usb_impl_dir}/usb.c"
    INCLUDE_DIRS "${p4_usb_impl_dir}" "${CMAKE_SOURCE_DIR}"
    REQUIRES
        ansi
        fatfs
        usb
        usb_host_hid
        usb_host_msc
)