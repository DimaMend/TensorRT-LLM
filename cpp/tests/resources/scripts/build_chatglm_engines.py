#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import platform
import shutil
import sys
import typing
from pathlib import Path

from build_engines_utils import init_model_spec_module, run_command, wincopy

init_model_spec_module()
import model_spec

import tensorrt_llm.bindings as _tb

resources_dir = Path(__file__).parent.resolve().parent
model_dir = resources_dir / "models"
chatglm_example_dir = Path("examples/chatglm")
bCopyModel = True  # "False" to remove redundant copy of model from model_cache


def convert_ckpt(model_dir: str, output_dir: str, world_size: int):
    convert_cmd = [
        sys.executable,
        str(chatglm_example_dir / "convert_checkpoint.py"), "--dtype=float16",
        f"--model_dir={model_dir}", f"--output_dir={output_dir}",
        f"--tp_size={world_size}"
    ]
    run_command(convert_cmd)


def build_engine(ckpt_dir: str,
                 engine_dir: str,
                 is_ifb: bool = False,
                 is_chatglm_6b_or_glm_10b: bool = False):
    build_cmd = [
        "trtllm-build",
        f"--checkpoint_dir={ckpt_dir}",
        f"--output_dir={engine_dir}",
        "--log_level=error",
        "--max_batch_size=8",
        "--max_beam_width=2",
        "--max_input_len=256",
        "--max_seq_len=384",
        "--gpt_attention_plugin=float16",
        "--gemm_plugin=float16",
        "--builder_opt=0",
    ]
    if is_ifb:
        build_cmd.extend([
            "--remove_input_padding=enable",
            "--paged_kv_cache=enable",
            "--context_fmha=enable",
            "--context_fmha_fp32_acc=enable",
            "--use_paged_context_fmha=enable",
        ])
    else:
        build_cmd.extend([
            "--remove_input_padding=disable",
            "--paged_kv_cache=disable",
        ])

    if is_chatglm_6b_or_glm_10b:
        print("Disable Context FMHA for ChatGLM-6B and GLM-10B")
        build_cmd.extend(
            ["--context_fmha=disable", "--context_fmha_fp32_acc=disable"])

    run_command(build_cmd)


def build_engines(model_cache: typing.Optional[str] = None,
                  world_size: int = 1):

    for model_name in ["chatglm-6b", "chatglm2-6b", "chatglm3-6b", "glm-10b"]:
        is_chatglm_6b_or_glm_10b = model_name in ["chatglm-6b", "glm-10b"]
        if model_cache and (Path(model_cache) / model_name).is_dir():
            model_cache_dir = Path(model_cache) / model_name
            if bCopyModel or model_name == "chatglm-6b":
                print("Copy model from model_cache")
                hf_dir = model_dir / model_name
                if platform.system() == "Windows":
                    wincopy(source=str(model_cache_dir),
                            dest=model_name,
                            isdir=True,
                            cwd=model_dir)
                else:
                    run_command(["rsync", "-av",
                                 str(model_cache_dir), "."],
                                cwd=model_dir)
            else:
                print("Use model from model_cache directly except ChatGLM-6B")
                hf_dir = Path(model_cache)

        else:
            hf_dir = model_dir / model_name
            if not hf_dir.is_dir():
                print("Clone model from HF")
                run_command(
                    [
                        "git", "clone",
                        f"https://huggingface.co/THUDM/{model_name}", model_name
                    ],
                    cwd=model_dir,
                )

        # Build engines
        print(f"Building {model_name}")
        ckpt_dir = Path(model_dir) / "c-model" / model_name
        ckpt_dir.mkdir(parents=True, exist_ok=True)

        # Fix HF error in ChatGLM-6B, hope to remove this in the future
        if model_name == "chatglm-6b":
            shutil.copy(
                chatglm_example_dir / "tokenization_chatglm.py",
                hf_dir,
            )

        convert_ckpt(hf_dir, ckpt_dir, world_size)

        model_spec_obj = model_spec.ModelSpec('input_tokens.npy',
                                              _tb.DataType.HALF)
        model_spec_obj.set_kv_cache_type(model_spec.KVCacheType.CONTINUOUS)
        model_spec_obj.use_gpt_plugin()
        engine_dir = Path(
            model_dir
        ) / "rt_engine" / model_name / model_spec_obj.get_model_path(
        ) / "tp1-pp1-gpu"
        engine_dir.mkdir(parents=True, exist_ok=True)
        build_engine(ckpt_dir, engine_dir, False, is_chatglm_6b_or_glm_10b)

        model_spec_obj.use_packed_input()
        model_spec_obj.set_kv_cache_type(model_spec.KVCacheType.PAGED)
        engine_dir = Path(
            model_dir
        ) / "rt_engine" / model_name / model_spec_obj.get_model_path(
        ) / "tp1-pp1-gpu"
        engine_dir.mkdir(parents=True, exist_ok=True)
        build_engine(ckpt_dir, engine_dir, True, is_chatglm_6b_or_glm_10b)

    print("Done")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model_cache",
                        type=str,
                        help="Directory where models are stored")

    parser.add_argument('--world_size',
                        type=int,
                        default=1,
                        help='world size, only support tensor parallelism now')

    build_engines(**vars(parser.parse_args()))
