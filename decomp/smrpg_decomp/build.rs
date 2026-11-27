// Finds the generated module (tools/decompgen). It is derived from the game binary, so it lives outside Git:
// SMRPG_DECOMPGEN_DIR names the directory, defaulting to the work directory on A:.
use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo::rustc-check-cfg=cfg(smrpg_gen)");
    println!("cargo::rerun-if-changed=build.rs");
    println!("cargo::rerun-if-env-changed=SMRPG_DECOMPGEN_DIR");
    let dir = env::var_os("SMRPG_DECOMPGEN_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"A:\AI\Recomps\smrpg-work\decompgen"));
    let file = dir.join("gen.rs");
    println!("cargo::rerun-if-changed={}", file.display());
    if file.is_file() {
        println!("cargo::rustc-cfg=smrpg_gen");
        println!("cargo::rustc-env=SMRPG_GEN_RS={}", file.display());
    }
}
