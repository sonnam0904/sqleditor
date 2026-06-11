add_library(
    tree-sitter-sql-grammar
    STATIC
    external/tree-sitter-sql/src/parser.c
    external/tree-sitter-sql/src/scanner.c
)
target_include_directories(
    tree-sitter-sql-grammar
    PRIVATE external/tree-sitter-sql/src
)
target_link_libraries(
    tree-sitter-sql-grammar
    PRIVATE unofficial::tree-sitter::tree-sitter
)
set_target_properties(tree-sitter-sql-grammar PROPERTIES C_STANDARD 11)

add_library(
    tree-sitter-json-grammar
    STATIC
    external/tree-sitter-json/src/parser.c
)
target_include_directories(
    tree-sitter-json-grammar
    PRIVATE external/tree-sitter-json/src
)
target_link_libraries(
    tree-sitter-json-grammar
    PRIVATE unofficial::tree-sitter::tree-sitter
)
set_target_properties(tree-sitter-json-grammar PROPERTIES C_STANDARD 11)
