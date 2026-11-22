fn main() -> Result<(), slint_build::CompileError> {
    slint_build::compile_with_config(
        "panel.slint",
        slint_build::CompilerConfiguration::new().with_style("fluent".into()),
    )
}
