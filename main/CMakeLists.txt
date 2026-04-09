set(WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(WEB_DIST_DIR "${WEB_DIR}/dist")

idf_component_register(
    SRCS
        "uni-gtw.c"
        "webserver.c"
        "cc1101.c"
        "radio.c"
        "channel.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_server
        nvs_flash
        esp_event
        esp_netif
        esp_driver_spi
        esp_driver_gpio
        esp_hw_support
        cosmo
        esp_littlefs
        lwip
    EMBED_FILES
        ${WEB_DIST_DIR}/index.html
        ${WEB_DIST_DIR}/app.js
        ${WEB_DIST_DIR}/app.css
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_custom_target(pnpm_build
        COMMAND pnpm install --frozen-lockfile
        COMMAND pnpm run build
        WORKING_DIRECTORY ${WEB_DIR}
        BYPRODUCTS
            ${WEB_DIST_DIR}/index.html
            ${WEB_DIST_DIR}/app.js
            ${WEB_DIST_DIR}/app.css
    )

    add_dependencies(${COMPONENT_LIB} pnpm_build)
endif()
