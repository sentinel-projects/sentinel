fn main() {
    let target = std::env::var("TARGET").expect("Cargo supplies the target triple");
    println!("cargo:rustc-env=SENTINEL_CLI_TARGET={target}");
}
