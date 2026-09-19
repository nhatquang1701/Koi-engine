//! Trains a Koi NNUE network with bullet.
//!
//! `--arch v5` (the default) mirrors the C++ v5 inference path: one shared
//! 36864-input sparse affine feature transformer over both perspectives,
//! CReLU, full-width cross-perspective pair products, one 32-unit CReLU hidden
//! layer, and one linear output per piece-count bucket. `--arch v4` keeps the
//! original 9216-input network with within-perspective pair products so the
//! existing export path stays available. Targets are centipawns divided by 100
//! (`eval_scale = 100`), so the raw network output is directly quantizable to
//! the matching `KOI-NNUE` container version.

use std::path::PathBuf;
use std::process::ExitCode;

use bullet_lib::nn::optimiser::AdamW;
use bullet_lib::trainer::save::SavedFormat;
use bullet_lib::trainer::schedule::{lr, wdl, TrainingSchedule, TrainingSteps};
use bullet_lib::trainer::settings::LocalSettings;
use bullet_lib::value::loader::DirectSequentialDataLoader;
use bullet_lib::value::ValueTrainerBuilder;
use koi_bullet_train::{
    KoiHalfkaKingBucket, KoiHalfkaThreat, KoiOutputBuckets, KOI_INPUTS, KOI_OUTPUT_BUCKETS,
    KOI_V5_INPUTS,
};

struct Config {
    data: PathBuf,
    test: Option<PathBuf>,
    out: PathBuf,
    net_id: String,
    arch: String,
    hidden: usize,
    l1_units: usize,
    batch: usize,
    batches_per_superbatch: usize,
    superbatches: usize,
    lr: f32,
    final_lr: f32,
    seed: u64,
    threads: usize,
    save_rate: usize,
}

fn usage() -> String {
    [
        "usage: bullet_train --data <file.data> --out <dir> [options]",
        "",
        "options:",
        "  --test <file.data>            validation dataset (optional)",
        "  --arch <v4|v5>                architecture (default v5)",
        "  --net-id <name>               checkpoint name (default koi-v5)",
        "  --hidden <n>                  hidden units, even (default 1536 for v5, 1024 for v4)",
        "  --l1-units <n>                v5 hidden layer units (default 32)",
        "  --batch <n>                   batch size (default 8192)",
        "  --batches-per-superbatch <n>  batches per superbatch (default 610)",
        "  --superbatches <n>            total superbatches (default 100)",
        "  --lr <f>                      initial learning rate (default 0.002)",
        "  --final-lr <f>                final learning rate (default 0.0002)",
        "  --seed <n>                    rng seed (default 20260916)",
        "  --threads <n>                 data loader threads (default 4)",
        "  --save-rate <n>               save every N superbatches (default 10)",
    ]
    .join("\n")
}

fn parse_args() -> Result<Config, String> {
    let mut data = None;
    let mut test = None;
    let mut out = None;
    let mut arch = String::from("v5");
    let mut net_id = String::from("koi-v5");
    let mut net_id_set = false;
    let mut hidden = 1536usize;
    let mut hidden_set = false;
    let mut l1_units = 32usize;
    let mut batch = 8192usize;
    let mut batches_per_superbatch = 610usize;
    let mut superbatches = 100usize;
    let mut lr = 0.002f32;
    let mut final_lr = 0.0002f32;
    let mut seed = 20260916u64;
    let mut threads = 4usize;
    let mut save_rate = 10usize;

    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut index = 0;
    while index < args.len() {
        let flag = args[index].as_str();
        if flag == "--help" || flag == "-h" {
            return Err(usage());
        }
        let value = args
            .get(index + 1)
            .ok_or_else(|| format!("missing value for {flag}"))?;
        match flag {
            "--data" => data = Some(PathBuf::from(value)),
            "--test" => test = Some(PathBuf::from(value)),
            "--out" => out = Some(PathBuf::from(value)),
            "--arch" => arch = value.clone(),
            "--net-id" => {
                net_id = value.clone();
                net_id_set = true;
            }
            "--hidden" => {
                hidden = value.parse().map_err(|_| "bad --hidden".to_string())?;
                hidden_set = true;
            }
            "--l1-units" => {
                l1_units = value.parse().map_err(|_| "bad --l1-units".to_string())?
            }
            "--batch" => batch = value.parse().map_err(|_| "bad --batch".to_string())?,
            "--batches-per-superbatch" => {
                batches_per_superbatch = value
                    .parse()
                    .map_err(|_| "bad --batches-per-superbatch".to_string())?
            }
            "--superbatches" => {
                superbatches = value
                    .parse()
                    .map_err(|_| "bad --superbatches".to_string())?
            }
            "--lr" => lr = value.parse().map_err(|_| "bad --lr".to_string())?,
            "--final-lr" => final_lr = value.parse().map_err(|_| "bad --final-lr".to_string())?,
            "--seed" => seed = value.parse().map_err(|_| "bad --seed".to_string())?,
            "--threads" => threads = value.parse().map_err(|_| "bad --threads".to_string())?,
            "--save-rate" => {
                save_rate = value.parse().map_err(|_| "bad --save-rate".to_string())?
            }
            other => return Err(format!("unknown argument {other}\n\n{}", usage())),
        }
        index += 2;
    }

    if arch != "v4" && arch != "v5" {
        return Err("--arch must be v4 or v5".to_string());
    }
    if arch == "v4" {
        if !hidden_set {
            hidden = 1024;
        }
        if !net_id_set {
            net_id = String::from("koi-v4");
        }
    }
    if hidden < 32 || hidden % 2 != 0 || hidden > 8192 {
        return Err("--hidden must be an even number between 32 and 8192".to_string());
    }
    if l1_units < 8 || l1_units > 128 {
        return Err("--l1-units must be between 8 and 128".to_string());
    }
    if batch == 0 || superbatches == 0 || batches_per_superbatch == 0 || save_rate == 0 {
        return Err("--batch, --batches-per-superbatch, --superbatches and --save-rate must be positive".to_string());
    }

    Ok(Config {
        data: data.ok_or_else(|| format!("--data is required\n\n{}", usage()))?,
        test,
        out: out.ok_or_else(|| format!("--out is required\n\n{}", usage()))?,
        net_id,
        arch,
        hidden,
        l1_units,
        batch,
        batches_per_superbatch,
        superbatches,
        lr,
        final_lr,
        seed,
        threads,
        save_rate,
    })
}

fn main() -> ExitCode {
    let config = match parse_args() {
        Ok(config) => config,
        Err(message) => {
            eprintln!("{message}");
            return ExitCode::from(2);
        }
    };

    if let Err(error) = std::fs::create_dir_all(&config.out) {
        eprintln!("bullet_train: cannot create {}: {error}", config.out.display());
        return ExitCode::from(1);
    }

    println!(
        "bullet training start net={} hidden={} batch={} batches_per_superbatch={} superbatches={}",
        config.net_id, config.hidden, config.batch, config.batches_per_superbatch, config.superbatches
    );

    let schedule = TrainingSchedule {
        net_id: config.net_id.clone(),
        eval_scale: 100.0,
        steps: TrainingSteps {
            batch_size: config.batch,
            batches_per_superbatch: config.batches_per_superbatch,
            start_superbatch: 1,
            end_superbatch: config.superbatches,
        },
        wdl_scheduler: wdl::ConstantWDL { value: 0.0 },
        lr_scheduler: lr::CosineDecayLR {
            initial_lr: config.lr,
            final_lr: config.final_lr,
            final_superbatch: config.superbatches,
        },
        save_rate: config.save_rate,
    };

    let test_path = config.test.as_ref().map(|path| path.to_string_lossy().into_owned());
    let output_directory = config.out.to_string_lossy().into_owned();
    let settings = LocalSettings {
        threads: config.threads,
        test_set: test_path
            .as_deref()
            .map(|path| bullet_lib::trainer::settings::TestDataset { path, freq: 10 }),
        output_directory: output_directory.as_str(),
        batch_queue_size: 32,
    };

    let data_path = config.data.to_string_lossy().into_owned();
    let loader = DirectSequentialDataLoader::new(&[data_path.as_str()]);

    if config.arch == "v5" {
        let save_format = [
            SavedFormat::id("l0w"),
            SavedFormat::id("l0b"),
            SavedFormat::id("l1w"),
            SavedFormat::id("l1b"),
            SavedFormat::id("l2w"),
            SavedFormat::id("l2b"),
        ];

        let mut trainer = ValueTrainerBuilder::default()
            .optimiser(AdamW)
            .inputs(KoiHalfkaThreat)
            .dual_perspective()
            .output_buckets(KoiOutputBuckets)
            .save_format(&save_format)
            .loss_fn(|output, target| output.sigmoid().squared_error(target))
            .seed(config.seed)
            .build(|builder, stm_inputs, ntm_inputs, output_buckets| {
                let l0 = builder.new_affine("l0", KOI_V5_INPUTS, config.hidden);
                let stm_hidden = l0.forward(stm_inputs).crelu();
                let ntm_hidden = l0.forward(ntm_inputs).crelu();
                let pairs = stm_hidden * ntm_hidden;
                let l1 = builder.new_affine("l1", config.hidden, config.l1_units);
                let l1_act = l1.forward(pairs).crelu();
                let l2 = builder.new_affine("l2", config.l1_units, KOI_OUTPUT_BUCKETS);
                l2.forward(l1_act).select(output_buckets)
            });

        trainer.run(&schedule, &settings, &loader);
    } else {
        let save_format = [
            SavedFormat::id("l0w"),
            SavedFormat::id("l0b"),
            SavedFormat::id("l1w"),
            SavedFormat::id("l1b"),
        ];

        let mut trainer = ValueTrainerBuilder::default()
            .optimiser(AdamW)
            .inputs(KoiHalfkaKingBucket)
            .output_buckets(KoiOutputBuckets)
            .save_format(&save_format)
            .loss_fn(|output, target| output.sigmoid().squared_error(target))
            .seed(config.seed)
            .build(|builder, stm_inputs, output_buckets| {
                let l0 = builder.new_affine("l0", KOI_INPUTS, config.hidden);
                let l1 = builder.new_affine("l1", config.hidden / 2, KOI_OUTPUT_BUCKETS);
                let first_half = |start: usize, end: usize| {
                    l0.slice(start, end).forward(stm_inputs).crelu()
                };
                let pairs = first_half(0, config.hidden / 2)
                    * first_half(config.hidden / 2, config.hidden);
                l1.forward(pairs).select(output_buckets)
            });

        trainer.run(&schedule, &settings, &loader);
    }

    println!("bullet training finished net={}", config.net_id);
    ExitCode::SUCCESS
}
