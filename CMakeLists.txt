cmake_minimum_required(VERSION 3.14)
project(ArasAI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. llama.cpp alt projesini dahil et
add_subdirectory(llama.cpp)

# 2. İşletim sistemine özel pencere ve konsol ayarları (Terminal gizleme)
if (APPLE)
    # macOS için uygulama paketi (.app) oluşturur ve arka plandaki terminali engeller
    add_executable(chat_app MACOSX_BUNDLE main.cpp)
elseif (WIN32)
    # Windows için siyah komut istemi penceresini gizler (GUI modu)
    add_executable(chat_app WIN32 main.cpp)
    target_link_options(chat_app PRIVATE "-mwindows")
else()
    # Linux ve diğer Unix sistemler için standart çalıştırılabilir dosya
    add_executable(chat_app main.cpp)
endif()

# 3. Raylib kütüphanesini bul (Sisteminde kurulu olmalıdır)
find_package(raylib REQUIRED)

# 4. Gerekli kütüphaneleri hedefe bağla (linking)
target_link_libraries(chat_app PRIVATE llama raylib)

# 5. macOS ve Apple Silicon için grafik framework bağımlılıkları
if (APPLE)
    target_link_libraries(chat_app PRIVATE 
        "-framework OpenGL" 
        "-framework Cocoa" 
        "-framework IOKit" 
        "-framework CoreVideo"
    )
endif()