fn main() {
    let source = std::path::Path::new("src");
    cc::Build::new()
        .include(source)
        .file(source.join("parser.c"))
        .define("TREE_SITTER_HIDE_SYMBOLS", None)
        .warnings(false)
        .compile("tree-sitter-dm-sql");
    println!("cargo:rerun-if-changed=src/parser.c");
    println!("cargo:rerun-if-changed=src/tree_sitter");
}
