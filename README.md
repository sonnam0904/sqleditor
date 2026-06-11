
<h1 align="center">SQLEditor</h1>

<p align="center">A simple, cross-platform database client.</p>


## Features

- Support SQLite, PostgreSQL, MySQL, MariaDB, MongoDB, Redis, Microsoft SQL Server, Oracle, Amazon Redshift, and Cassandra
- Cross-platform: macOS (Metal), Linux (GTK4 + OpenGL), Windows (DirectX 11)
- Database browser: sidebar for exploring schemas, tables, views, and routines
- SQL editor with syntax highlighting, autocomplete, formatting, and AI assistant
- Table viewer and editor with sorting, filtering, and export (CSV, JSON, SQL)
- ER diagram view for visualizing table relationships
- Native file dialogs for SQLite and CSV files
- Connection URLs, SSH tunnels, and SSL/TLS configuration
- Saves and restores previous database connections
- Query history with execution time and row counts

## Installation

Download pre-built binaries from [GitHub Releases](https://github.com/sonnam0904/sqleditor/releases).

| Platform | File |
|----------|------|
| Linux (x86_64) | `SQLEditor-x86_64.AppImage` |
| Windows (x64) | `SQLEditor-x64.msi` |
 
## Build from source

### Prerequisites

- CMake 3.20+
- C++20 compiler (GCC, Clang, or MSVC)
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set
- Git (for submodules)

### Quick start

```sh
git clone --recursive git@github.com:sonnam0904/sqleditor.git
cd sqleditor

# Install system dependencies, init submodules, download SQLite amalgamation
./scripts/setup

# Build (Debug)
./scripts/build

# Build release (Linux also produces AppImage)
./scripts/build release
```

### vcpkg setup

If you do not have vcpkg yet:

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

### Run after building

| Platform | Debug | Release |
|----------|-------|---------|
| Linux | `./build/SQLEditor` | `./build_release/SQLEditor` |
| macOS | `open build/SQLEditor.app` | `open build_release/SQLEditor.app` |
| Windows | `.\build\SQLEditor.exe` | `.\build_release\SQLEditor.exe` |

### Platform-specific build scripts

```sh
# macOS (signed release + DMG)
./scripts/build-mac

# Windows (PowerShell, MSVC required)
.\scripts\build-windows.ps1
```

## Usage

### Getting started

1. Launch SQLEditor.
2. Click **+** in the title bar, or right-click the sidebar and choose **Add Database Connection**.
3. Fill in the connection form (or paste a connection URL — see below).
4. Click **Connect**. The database tree appears in the sidebar once connected.

You can also open a SQLite file directly by double-clicking `.db`, `.sqlite`, or `.sqlite3` files (when associated with SQLEditor), or drag-and-drop them onto the window.

### Adding a connection

The connection dialog supports:

| Field | Description |
|-------|-------------|
| **Type** | SQLite, PostgreSQL, MySQL, MariaDB, MongoDB, Redis, MSSQL, Oracle, Redshift, Cassandra |
| **Host / Port** | Server address (not used for SQLite) |
| **Database** | Database or service name |
| **Username / Password** | Credentials |
| **SSL** | `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full` |
| **SSH tunnel** | Route the connection through an SSH jump host |
| **Show all databases** | List every database on the server in the sidebar |

**Connection URLs** — paste a URL into the URL field to auto-fill the form:

```
sqlite:///path/to/file.db
postgresql://user:pass@localhost:5432/mydb
mysql://user:pass@localhost:3306/mydb
mongodb://user:pass@localhost:27017/mydb
redis://localhost:6379
rediss://localhost:6380          # Redis with TLS
mssql://user:pass@localhost:1433/mydb
oracle://user:pass@localhost:1521/XEPDB1
redshift://user:pass@host:5439/dev
```

Append query parameters for SSL, e.g. `?sslmode=require` or `?sslrootcert=/path/to/ca.pem`.

> **Note:** The free tier is limited to 3 saved connections. Activate a license to add more.

### Sidebar

The sidebar has two panels:

- **Structure** — browse databases, schemas, tables, views, routines, and sequences. Expand nodes to explore; double-click a table or view to open its data.
- **History** — recent queries with type badges, row counts, and execution time. Right-click an entry to copy the query to the clipboard.

**Context menus** (right-click):

- Empty sidebar → Add connection, open CSV file
- Database node → New SQL Editor, Show Diagram, Edit/Rename/Disconnect/Remove, Refresh
- Table(s) → View Data, Export (CSV / JSON / SQL), Delete

### SQL Editor

Open a SQL Editor tab from a database's context menu or by double-clicking a routine.

| Action | How |
|--------|-----|
| **Run query** | Click **Run**, or press `Ctrl+Enter` / `Cmd+Enter` |
| **Run selection** | Select text, then click **Run** or `Ctrl+Enter` |
| **Format SQL** | Click **Format** in the toolbar |
| **Save script** | `Ctrl+S` / `Cmd+S` |
| **Autocomplete** | `Ctrl+.` or `Ctrl+Space` |
| **Find** | `Ctrl+F` |
| **Find & replace** | `Ctrl+H` |
| **Toggle comment** | `Ctrl+/` |
| **Cancel query** | Click **Cancel** while a query is running |

Query results appear below the editor (up to 1,000 rows). Multiple result sets are shown in tabs.

The **AI panel** (toggle on the right edge of the editor) can help write, explain, or fix SQL using your database schema as context. Configure the AI provider in the settings dialog.

### Table viewer

Double-click any table or view in the sidebar to open a read-only data tab.

- Click column headers to sort (right-click for ASC/DESC)
- Right-click cells to copy, filter, edit, or delete rows (when supported)
- Double-click a cell to edit inline (on supported databases)
- Export data from the table toolbar

### Other tabs

| Tab | Open from |
|-----|-----------|
| **Table Editor** | Right-click a table → Edit Table |
| **ER Diagram** | Database context menu → Show Diagram |
| **CSV Editor** | Right-click sidebar → Open CSV File |
| **Redis / MongoDB editors** | Double-click keys or collections in the sidebar |

### Workspaces

SQLEditor remembers your connections, open tabs, and window layout between sessions. Each workspace keeps its own set of saved connections.

## Development

### Run tests

Integration tests spin up Docker containers for PostgreSQL, MySQL, Redis, MongoDB, MSSQL, Oracle, Redshift, Cassandra, and SSH tunnel scenarios:

```sh
./scripts/run-tests
```

Requires Docker and a configured build directory (`./scripts/build` first).

### Format code

```sh
./scripts/format
```

## Built With

- [Dear ImGui](https://github.com/ocornut/imgui) — Immediate mode GUI
- [Native File Dialog](https://github.com/btzy/nativefiledialog-extended) — File pickers
- [IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) — Icon fonts
- [vcpkg.json](vcpkg.json) — Full dependency list
