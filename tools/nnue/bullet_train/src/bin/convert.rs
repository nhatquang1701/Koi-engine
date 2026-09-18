//! Converts `bulletformat` text positions (`<FEN> | <score> | <result>`) to a
//! binary `.data` file. This binary does not need the `cuda` feature.

use std::process::ExitCode;

use bulletformat::{convert_from_text, ChessBoard};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: convert <input.txt> <output.data>");
        return ExitCode::from(2);
    }

    match convert_from_text::<ChessBoard>(&args[1], &args[2]) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("convert: {error}");
            ExitCode::from(1)
        }
    }
}
