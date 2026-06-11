add_library(
    sql-parser
    STATIC
    external/sql-parser/src/SQLParser.cpp
    external/sql-parser/src/SQLParserResult.cpp
    external/sql-parser/src/sql/CreateStatement.cpp
    external/sql-parser/src/sql/Expr.cpp
    external/sql-parser/src/sql/PrepareStatement.cpp
    external/sql-parser/src/sql/SQLStatement.cpp
    external/sql-parser/src/sql/statements.cpp
    external/sql-parser/src/util/sqlhelper.cpp
    external/sql-parser/src/parser/bison_parser.cpp
    external/sql-parser/src/parser/flex_lexer.cpp
)
target_include_directories(sql-parser PUBLIC external/sql-parser/src)
if(WIN32)
  target_compile_definitions(
      sql-parser
      PRIVATE
          YY_NO_UNISTD_H
          strcasecmp=_stricmp
          strncasecmp=_strnicmp
  )
endif()
set_target_properties(
    sql-parser
    PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
)
if(NOT MSVC)
  set_source_files_properties(
      external/sql-parser/src/parser/bison_parser.cpp
      PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable"
  )
  set_source_files_properties(
      external/sql-parser/src/parser/flex_lexer.cpp
      PROPERTIES
          COMPILE_OPTIONS
              "-Wno-sign-compare;-Wno-unneeded-internal-declaration;-Wno-register"
  )
endif()
