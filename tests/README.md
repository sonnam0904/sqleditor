# SQLEditor Tests

Thư mục này chứa các bài test cho SQLEditor:

| Loại | Target CMake | Mô tả |
|------|--------------|-------|
| Database integration | `database_tests` | Kết nối thật tới PostgreSQL, MySQL, Redis, MongoDB, MSSQL, Oracle, Cassandra, SSH tunnel, SSL |
| UI / parser | `sql_format_tests` | Format SQL/JSON, CSV parser |
| UI automation | `tests/ui/` | ImGui Test Engine (mã nguồn; chưa gắn vào CMake mặc định) |

---

## Yêu cầu

- **CMake** ≥ 3.23, **C++20** compiler
- **[vcpkg](https://github.com/microsoft/vcpkg)** — baseline trong `vcpkg.json`
- **Git submodules** — imgui, freetds, cassandra-cpp-driver, …
- **Docker** — cho `scripts/run-tests` (tự khởi động container test)
- **Linux**: GTK4, FreeTDS, autotools (xem bên dưới)

### Cài dependency hệ thống (Linux)

Cách nhanh nhất — chạy script setup (cài package + init submodule + tải SQLite amalgamation):

```bash
./scripts/setup
```

Hoặc cài thủ công trên Ubuntu/Debian:

```bash
sudo apt-get install -y \
  build-essential cmake pkg-config bison flex \
  autoconf autoconf-archive automake libtool \
  libgtk-4-dev libepoxy-dev libx11-dev \
  freetds-dev libgnutls28-dev libkrb5-dev
```

### vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg   # thêm vào ~/.bashrc nếu cần
```

### Submodules

```bash
git submodule update --init --recursive
```

---

## Build

### Build ứng dụng

```bash
# Debug (mặc định) → build/SQLEditor
./scripts/build

# Release → build_release/SQLEditor
./scripts/build release

# Cấu hình lại sau khi đổi dependency hoặc vcpkg
./scripts/build --reconfigure
```

Binary nằm tại `build/SQLEditor` (Linux) hoặc `build/SQLEditor.app` (macOS).

### Chỉ build test

```bash
cmake -S . -B build
cmake --build build --target database_tests sql_format_tests
```

Hoặc build tất cả target đã đăng ký với CTest:

```bash
cmake --build build
```

---

## Chạy test

### Cách khuyến nghị — script tự động

`scripts/run-tests` sẽ:

1. Tạo TLS cert và khởi động container Docker (Postgres, MySQL, Redis, MongoDB, MSSQL, Oracle, Cassandra, SSH)
2. Export biến môi trường `SQLEDITOR_TEST_*`
3. Build `database_tests` và `sql_format_tests`
4. Chạy cả hai binary

```bash
./scripts/run-tests
```

Truyền tham số GTest xuống `database_tests` (ví dụ lọc test):

```bash
./scripts/run-tests --gtest_filter='Postgres*'
```

Nếu các cổng test đã mở sẵn, script bỏ qua bước Docker và chạy test trực tiếp.

### Chạy thủ công

```bash
# Cần Docker đã chạy hoặc tự export SQLEDITOR_TEST_* trỏ tới DB thật
./scripts/run-tests          # hoặc chỉ setup container rồi dừng trước bước test

cd build
./database_tests
./sql_format_tests
```

Lọc test cụ thể:

```bash
./build/database_tests --gtest_filter='MySQL*'
./build/database_tests --gtest_list_tests   # xem danh sách
./build/sql_format_tests --gtest_filter='SqlFormat*'
```

### CTest

```bash
cd build
ctest --output-on-failure

# Chạy một target
ctest -R database_tests --output-on-failure
```

### Biến môi trường test chính

| Biến | Mô tả |
|------|-------|
| `SQLEDITOR_TEST_PG_HOST` / `_PORT` / `_DB` / `_USER` / `_PASSWORD` | PostgreSQL |
| `SQLEDITOR_TEST_MYSQL_*` | MySQL |
| `SQLEDITOR_TEST_REDIS_*` | Redis (có `_TLS_PORT` cho SSL) |
| `SQLEDITOR_TEST_MONGODB_*` | MongoDB |
| `SQLEDITOR_TEST_MSSQL_*` | SQL Server (FreeTDS) |
| `SQLEDITOR_TEST_ORACLE_*` | Oracle |
| `SQLEDITOR_TEST_CASSANDRA_*` | Cassandra |
| `SQLEDITOR_TEST_SSH_*` | SSH tunnel test |
| `SQLEDITOR_TEST_CA_CERT` | CA cert cho kết nối TLS |
| `SQLEDITOR_TEST_ORACLE_INSTALLER_DOWNLOAD=1` | Bật test tải Oracle Instant Client |

Cổng Docker mặc định có thể ghi đè, ví dụ: `POSTGRES_PORT=55432 ./scripts/run-tests`.

### CI

Workflow `.github/workflows/linux-test.yml` chạy:

```bash
./scripts/run-tests
```

với timeout 30 phút mỗi test binary.

---

## Cấu trúc thư mục

```
tests/
├── database/          # Integration tests (GTest + DB thật)
│   ├── postgres_database_test.cpp
│   ├── mysql_database_test.cpp
│   ├── redis_database_test.cpp
│   └── ...
├── ui/
│   ├── sql_format_test.cpp
│   ├── csv_parser_test.cpp
│   ├── main_test.mm          # ImGui Test Engine entry (macOS/GLFW)
│   └── sidebar_tests.cpp
└── README.md
```

---

## SQL UI Tests (ImGui Test Engine)

Phần dưới mô tả UI automation dùng [ImGui Test Engine](https://github.com/ocornut/imgui_test_engine). Mã nguồn nằm trong `tests/ui/`; target `ui_tests` chưa được thêm vào `cmake/Tests.cmake` — cần build thủ công hoặc tích hợp CMake trước khi dùng.

### Build & chạy (thủ công, khi đã có target)

```bash
cd build
cmake --build . --target ui_tests

# Có cửa sổ test engine
./ui_tests

# Headless / CI
./ui_tests -nopause
```

### Test files

- **`tests/ui/main_test.cpp`**: Test runner entry point
  - Initializes the application
  - Creates and configures ImGui Test Engine
  - Registers all test suites
  - Manages test execution loop
  - Reports results

- **`tests/ui/sidebar_tests.cpp`**: Sidebar UI tests
  - Sidebar visibility toggling
  - Database list display
  - Database selection
  - Tree node expansion
  - SQLite connection dialog
  - Multiple database handling
  - Workspace switching
  - Rendering stability

### Adding New Tests

To add a new test, register it in the appropriate test file:

```cpp
void RegisterMyTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = nullptr;

    t = IM_REGISTER_TEST(engine, "Category", "Test Name");
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        // Render your UI here
        auto& app = Application::getInstance();
        app.renderMainUI();
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Test logic here
        auto& app = Application::getInstance();

        // Simulate user actions
        ctx->Yield(); // Wait one frame

        // Check conditions
        IM_CHECK(app.isSidebarVisible() == true);
    };
}
```

Then register your test suite in `main_test.cpp`:

```cpp
// Add forward declaration
void RegisterMyTests(ImGuiTestEngine* engine);

// In main()
RegisterSidebarTests(engine);
RegisterMyTests(engine);  // Add this line
```

## Test API Reference

### Common Operations

```cpp
// Wait for frames
ctx->Yield();       // Wait 1 frame
ctx->Yield(5);      // Wait 5 frames

// Find and interact with UI elements
ctx->ItemClick("*/Button Label");
ctx->ItemOpen("*/TreeNode");
ctx->ItemClose("*/TreeNode");
ctx->ItemInput("*/InputField");

// Assertions
IM_CHECK(condition);
IM_CHECK_EQ(a, b);
IM_CHECK_NE(a, b);
```

### ImGui Test Engine Documentation

For more details on ImGui Test Engine API:
- [Official Wiki](https://github.com/ocornut/imgui_test_engine/wiki)
- See `external/imgui_test_engine/docs/` for documentation

## Current Test Coverage

### Sidebar Tests
- ✅ Toggle visibility
- ✅ Show database list
- ✅ Select database
- ✅ Expand tree nodes
- ✅ SQLite connection dialog workflow
- ✅ Multiple databases
- ✅ Workspace switching
- ✅ Basic rendering stability

### Future Test Ideas
- SQL editor tab interactions
- Table viewer functionality
- Query execution
- ER diagram rendering
- Tab management
- Connection persistence

## Troubleshooting

### `vcpkg install failed` (bison, autotools, …)

Cài thêm package hệ thống rồi `./scripts/build --reconfigure`:

```bash
sudo apt-get install -y bison flex autoconf autoconf-archive automake libtool
```

### `Could not find SYBDB_LIBRARY`

```bash
sudo apt-get install -y freetds-dev libgnutls28-dev libkrb5-dev
```

### `Package 'gtk4' not found`

```bash
sudo apt-get install -y libgtk-4-dev libepoxy-dev libx11-dev pkg-config
```

### `cassandra-cpp-driver` không có `CMakeLists.txt`

```bash
git submodule update --init --recursive external/cassandra-cpp-driver
```

### Tests fail to build
- Ensure imgui_test_engine submodule is initialized: `git submodule update --init --recursive`
- Check CMake version is 3.23+
- Verify all dependencies are installed via `./scripts/setup`

### Tests crash on startup
- Check that the application initializes correctly
- Verify ImGui context is created before test engine
- Look for error messages in console output

### Tests fail intermittently
- Increase `ctx->Yield()` waits for async operations
- Check for race conditions in async code
- Verify test isolation (each test should clean up)

### Docker container không khởi động

```bash
docker ps -a | grep sqleditor
docker logs sqleditor-postgres-test   # xem log từng container
```

Xóa container cũ nếu cổng bị chiếm:

```bash
docker rm -f sqleditor-postgres-test sqleditor-mysql-test sqleditor-redis-test \
  sqleditor-mongodb-test sqleditor-mssql-test sqleditor-oracle-test \
  sqleditor-cassandra-test sqleditor-ssh-test
```

## Notes

- `database_tests` cần DB thật hoặc container Docker — không phải unit test thuần
- Tests UI chạy trong application context thật (khi đã build `ui_tests`)
- Có thể chạy headless với `-nopause` cho CI
