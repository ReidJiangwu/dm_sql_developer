fn main() {
    if let Err(error) = reid_dm_language_server::run() {
        eprintln!("language server stopped: {error}");
        std::process::exit(1);
    }
}
