#!/usr/bin/env python3

"""Regenerate the audio examples and check them against committed WAV files."""

import argparse
from array import array
import hashlib
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import wave


MAX_ABSOLUTE_DELTA = 4
MAX_RMS_DELTA = 1.0
EXPECTED_SOURCE_SHA256 = (
    "e63743eb6440e3aa73f54b918e45924852722197626436376ec1e6182343d031"
)

EXAMPLES = (
    (
        "pitch-up-1.25.wav",
        ("pitch", "--ratio", "1.25"),
        "d727bd50b0e2a5677e0a26152dfbadb29e974ada9ab91bee18aad0116a58b516",
    ),
    (
        "pitch-down-0.80.wav",
        ("pitch", "--ratio", "0.80"),
        "cb13643738d6aa6a6d78bab7b9c149d30a43e079dbbed4477b7ccb182c755081",
    ),
    (
        "chorus.wav",
        (
            "chorus",
            "--delay-ms",
            "18",
            "--depth-ms",
            "8",
            "--rate-hz",
            "0.8",
            "--wet",
            "0.6",
        ),
        "503f4346710ef1925ea4f46b1cde04e4c64842c78c5da50f164114c0d28c3640",
    ),
    (
        "vocoder.wav",
        (
            "vocoder",
            "--carrier-hz",
            "110",
            "--attack-ms",
            "2",
            "--release-ms",
            "35",
            "--noise-mix",
            "0.25",
            "--gate-dbfs",
            "-60",
        ),
        "cb54b9c26421e2e69c026cd5e96e8752144c352e4511be5e8aaca78811bc2886",
    ),
)


class AudioExampleCheckError(Exception):
    """An example cannot be regenerated or compared."""


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--renderer",
        required=True,
        type=Path,
        help="path to the audio_example_renderer executable",
    )
    parser.add_argument(
        "--audio-dir",
        required=True,
        type=Path,
        help="directory containing source.wav and the committed examples",
    )
    return parser.parse_args()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_renderer(renderer, source, output, arguments):
    command = [str(renderer), *arguments, str(source), str(output)]
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise AudioExampleCheckError(
            f"cannot run renderer {renderer}: {error}"
        ) from error

    if result.returncode != 0:
        diagnostics = []
        if result.stdout.strip():
            diagnostics.append(f"stdout: {result.stdout.strip()}")
        if result.stderr.strip():
            diagnostics.append(f"stderr: {result.stderr.strip()}")
        detail = "; ".join(diagnostics) or "no diagnostic output"
        raise AudioExampleCheckError(
            f"renderer exited with status {result.returncode}: {detail}"
        )
    if not output.is_file():
        raise AudioExampleCheckError(
            f"renderer succeeded but did not create {output.name}"
        )


def read_wav(path):
    try:
        with wave.open(str(path), "rb") as wav_file:
            parameters = wav_file.getparams()
            pcm_bytes = wav_file.readframes(parameters.nframes)
    except (OSError, EOFError, wave.Error) as error:
        raise AudioExampleCheckError(f"cannot read WAV file {path}: {error}") from error

    if parameters.comptype != "NONE":
        raise AudioExampleCheckError(
            f"{path} is compressed ({parameters.comptype}), expected PCM"
        )
    if parameters.sampwidth != 2:
        raise AudioExampleCheckError(
            f"{path} uses {parameters.sampwidth * 8}-bit samples, expected PCM16"
        )
    expected_bytes = parameters.nframes * parameters.nchannels * parameters.sampwidth
    if len(pcm_bytes) != expected_bytes:
        raise AudioExampleCheckError(
            f"{path} contains {len(pcm_bytes)} PCM bytes, expected {expected_bytes}"
        )

    samples = array("h")
    samples.frombytes(pcm_bytes)
    if sys.byteorder != "little":
        samples.byteswap()
    return parameters, samples


def format_metadata(parameters):
    channel_label = "mono" if parameters.nchannels == 1 else (
        "stereo" if parameters.nchannels == 2 else f"{parameters.nchannels} channels"
    )
    return (
        f"{parameters.nframes} frames, {channel_label}, "
        f"{parameters.framerate} Hz, PCM{parameters.sampwidth * 8}"
    )


def compare_wavs(committed_path, regenerated_path):
    committed_parameters, committed_samples = read_wav(committed_path)
    regenerated_parameters, regenerated_samples = read_wav(regenerated_path)

    if regenerated_parameters != committed_parameters:
        raise AudioExampleCheckError(
            "WAV metadata differs: "
            f"committed=({format_metadata(committed_parameters)}; "
            f"compression={committed_parameters.comptype!r}/"
            f"{committed_parameters.compname!r}), "
            f"regenerated=({format_metadata(regenerated_parameters)}; "
            f"compression={regenerated_parameters.comptype!r}/"
            f"{regenerated_parameters.compname!r})"
        )
    if len(regenerated_samples) != len(committed_samples):
        raise AudioExampleCheckError(
            "decoded sample counts differ despite matching WAV metadata"
        )
    if not committed_samples:
        raise AudioExampleCheckError("WAV contains no PCM samples")

    maximum_delta = 0
    squared_delta_sum = 0
    for committed, regenerated in zip(committed_samples, regenerated_samples):
        delta = regenerated - committed
        absolute_delta = abs(delta)
        if absolute_delta > maximum_delta:
            maximum_delta = absolute_delta
        squared_delta_sum += delta * delta
    rms_delta = math.sqrt(squared_delta_sum / len(committed_samples))

    summary = (
        f"{format_metadata(committed_parameters)}; "
        f"max |delta|={maximum_delta}, RMS delta={rms_delta:.6f}"
    )
    failures = []
    if maximum_delta > MAX_ABSOLUTE_DELTA:
        failures.append(
            f"maximum absolute PCM delta {maximum_delta} exceeds "
            f"{MAX_ABSOLUTE_DELTA}"
        )
    if rms_delta > MAX_RMS_DELTA:
        failures.append(
            f"RMS PCM delta {rms_delta:.6f} exceeds {MAX_RMS_DELTA:.1f}"
        )
    if failures:
        raise AudioExampleCheckError(f"{'; '.join(failures)} ({summary})")
    return summary


def validate_inputs(renderer, audio_dir):
    if not renderer.is_file():
        raise AudioExampleCheckError(f"renderer does not exist: {renderer}")
    if not audio_dir.is_dir():
        raise AudioExampleCheckError(f"audio directory does not exist: {audio_dir}")

    source = audio_dir / "source.wav"
    if not source.is_file():
        raise AudioExampleCheckError(f"source WAV does not exist: {source}")
    source_sha256 = sha256_file(source)
    if source_sha256 != EXPECTED_SOURCE_SHA256:
        raise AudioExampleCheckError(
            "source WAV SHA-256 differs from its provenance record: "
            f"measured {source_sha256}, expected {EXPECTED_SOURCE_SHA256}"
        )
    missing_examples = [
        name for name, _, _ in EXAMPLES if not (audio_dir / name).is_file()
    ]
    if missing_examples:
        raise AudioExampleCheckError(
            "committed example WAVs do not exist: " + ", ".join(missing_examples)
        )
    for name, _, expected_sha256 in EXAMPLES:
        measured_sha256 = sha256_file(audio_dir / name)
        if measured_sha256 != expected_sha256:
            raise AudioExampleCheckError(
                f"{name} SHA-256 differs from its provenance record: "
                f"measured {measured_sha256}, expected {expected_sha256}"
            )
    return source


def main():
    args = parse_args()
    renderer = args.renderer.resolve()
    audio_dir = args.audio_dir.resolve()

    try:
        source = validate_inputs(renderer, audio_dir)
    except AudioExampleCheckError as error:
        print(f"Audio example check failed: {error}", file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="voicechanger-audio-examples-") as temp:
        temp_dir = Path(temp)
        for filename, renderer_arguments, _ in EXAMPLES:
            regenerated = temp_dir / filename
            try:
                run_renderer(
                    renderer,
                    source,
                    regenerated,
                    renderer_arguments,
                )
                summary = compare_wavs(audio_dir / filename, regenerated)
            except AudioExampleCheckError as error:
                failures.append(f"{filename}: {error}")
                print(f"FAIL {filename}: {error}", file=sys.stderr)
            else:
                print(f"PASS {filename}: {summary}")

    if failures:
        print(
            f"Audio example check failed: {len(failures)} of "
            f"{len(EXAMPLES)} files did not match.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Audio example check passed: {len(EXAMPLES)} files match within "
        f"max |delta| <= {MAX_ABSOLUTE_DELTA} and "
        f"RMS delta <= {MAX_RMS_DELTA:.1f}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
