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
import json
import sys
from pathlib import Path
from typing import Optional, Union

import torch

from dataclasses import asdict, dataclass
from ..._utils import torch_dtype_to_str
from ...layers import MoeConfig
from ...logger import logger
from ...mapping import Mapping
from ..modeling_utils import PretrainedConfig, QuantConfig


"""
Configures replacing of the first k MoE MLPs with dense MLPs with the specified intermidiate size.
"""
@dataclass
class DenseReplaceConfig:
    first_k_moe_replace_by_dense: int = 0
    dense_intermediate_size: int = 0

    def validate(self) -> "DenseReplaceConfig":
        if (self.first_k_moe_replace_by_dense == 0) != (self.dense_intermediate_size == 0):
            raise ValueError(
                "Both or neither DenseReplaceConfig's first_k_moe_replace_by_dense and dense_intermediate_size must be set to 0"
            )
        return self
    
    def has_replace(self) -> bool:
        return self.first_k_moe_replace_by_dense > 0

    @classmethod
    def from_dict(cls, config: dict):
        return cls(**config)

    def to_dict(self):
        return asdict(self)


class LLaMAConfig(PretrainedConfig):

    def __init__(self,
                 *,
                 mlp_bias: bool = False,
                 attn_bias: bool = False,
                 rotary_base: float = 10000.0,
                 rotary_scaling: Optional[dict] = None,
                 residual_mlp: bool = False,
                 disable_weight_only_quant_plugin: bool = False,
                 moe: Optional[Union[MoeConfig, dict]] = None,
                 dense_replace: Optional[Union[DenseReplaceConfig, dict]] = None,
                 **kwargs):
        self.mlp_bias = mlp_bias
        self.attn_bias = attn_bias
        self.rotary_base = rotary_base
        self.rotary_scaling = rotary_scaling
        self.residual_mlp = residual_mlp
        self.disable_weight_only_quant_plugin = disable_weight_only_quant_plugin
        if moe is None:
            # Legacy MOE config fields
            moe = MoeConfig(
                num_experts=kwargs.pop('moe_num_experts', 0),
                top_k=kwargs.pop('moe_top_k', 0),
                tp_mode=kwargs.pop('moe_tp_mode',
                                   MoeConfig.ParallelismMode.TENSOR_PARALLEL),
                normalization_mode=kwargs.pop(
                    'moe_normalization_mode',
                    MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE),
                num_shared_experts=kwargs.pop(
                    'num_shared_experts',
                    0))
        elif isinstance(moe, dict):
            moe = MoeConfig.from_dict(moe)
        assert isinstance(moe, MoeConfig)
        self.moe = moe.validate()

        if dense_replace is None:
            dense_replace = DenseReplaceConfig(
                first_k_moe_replace_by_dense=0,
                dense_intermediate_size=0)
        elif isinstance(dense_replace, dict):
            dense_replace = DenseReplaceConfig.from_dict(dense_replace)
        assert isinstance(dense_replace, DenseReplaceConfig)
        self.dense_replace = dense_replace.validate()

        assert not self.dense_replace.has_replace() or self.moe.has_moe(), "DenseReplaceConfig can be used with moe only"

        super().__init__(**kwargs)

    def to_dict(self):
        output = super().to_dict()
        # Serialize the fields added in LLaMAConfig
        output['mlp_bias'] = self.mlp_bias
        output['attn_bias'] = self.attn_bias
        output['rotary_base'] = self.rotary_base
        output['rotary_scaling'] = self.rotary_scaling
        output['residual_mlp'] = self.residual_mlp
        output[
            'disable_weight_only_quant_plugin'] = self.disable_weight_only_quant_plugin
        output['moe'] = self.moe.to_dict()
        output['dense_replace'] = self.dense_replace.to_dict()
        return output

    @classmethod
    def from_hugging_face(
            cls,
            hf_config_or_dir: Union[str, 'transformers.PretrainedConfig'],
            dtype: str = 'auto',
            mapping: Optional[Mapping] = None,
            quant_config: Optional[QuantConfig] = None,
            **kwargs):
        import transformers

        if isinstance(hf_config_or_dir, transformers.PretrainedConfig):
            hf_config = hf_config_or_dir
        else:
            hf_config_dir = str(hf_config_or_dir)
            if "vila" in hf_config_dir:
                sys.path.append(hf_config_dir + "/../VILA")
                from llava.model import LlavaConfig, LlavaLlamaForCausalLM
                transformers.AutoConfig.register("llava_llama", LlavaConfig)
                transformers.AutoModelForCausalLM.register(
                    LlavaConfig, LlavaLlamaForCausalLM)

            hf_config = transformers.AutoConfig.from_pretrained(
                hf_config_dir, trust_remote_code=True)
            if hf_config.model_type == "llava":
                # LLaVA = Vision model + Llama LLM
                # We load a llava config and use its' text config as llama config
                hf_config = LlavaConfig.from_pretrained(
                    hf_config_dir).text_config
            if hf_config.model_type == "llava_llama":
                hf_config.llm_cfg["architecture"] = hf_config.llm_cfg[
                    "architectures"]
                hf_config.llm_cfg["dtype"] = hf_config.llm_cfg["torch_dtype"]
                hf_config = PretrainedConfig.from_dict(hf_config.llm_cfg)

        num_key_value_heads = getattr(hf_config, "num_key_value_heads",
                                      hf_config.num_attention_heads)
        hidden_act = hf_config.hidden_act
        attn_bias = getattr(hf_config, 'bias', False) or getattr(
            hf_config, 'attention_bias', False)
        rotary_scaling = getattr(hf_config, "rope_scaling", None)
        rotary_base = getattr(hf_config, "rope_theta", 10000.0)
        residual_mlp = getattr(hf_config, "parallel_attn_mlp_res", False)
        disable_weight_only_quant_plugin = kwargs.pop(
            'disable_weight_only_quant_plugin', False)

        if hf_config.model_type in ["mixtral", "arctic", "deepseek"]:
            # HF LLaMA-type models are implicitly using gated activation.
            # With our MoE implementation, we must make it explicit
            hidden_act = "swiglu"
            moe_normalization_mode = MoeConfig.ExpertScaleNormalizationMode.NONE if hf_config.model_type == "deepseek" and not hf_config.norm_topk_prob else MoeConfig.ExpertScaleNormalizationMode.RENORMALIZE
        else:
            moe_normalization_mode = None

        if hf_config.model_type == "deepseek":
            # deepseek has two intermediate sizes in config: for dense and for moe layers
            intermediate_size = hf_config.moe_intermediate_size
        else:
            intermediate_size = hf_config.intermediate_size

        if hf_config.model_type == "deepseek":
            # deepseek has two nums for experts in config: for routed and for shared experts
            moe_num_experts = hf_config.n_routed_experts
            moe_num_shared_experts = hf_config.n_shared_experts
        else:
            moe_num_experts = getattr(hf_config, "num_local_experts", 0)
            moe_num_shared_experts = 0
        moe_top_k = getattr(hf_config, "num_experts_per_tok", 0)
        moe_tp_mode = kwargs.pop('moe_tp_mode',
                                 MoeConfig.ParallelismMode.TENSOR_PARALLEL)
        moe_config = MoeConfig(num_experts=moe_num_experts,
                               top_k=moe_top_k,
                               tp_mode=moe_tp_mode,
                               normalization_mode=moe_normalization_mode,
                               num_shared_experts=moe_num_shared_experts)
        moe_config.validate()

        if hf_config.model_type == "deepseek":
            dense_replace = DenseReplaceConfig(
                first_k_moe_replace_by_dense=hf_config.first_k_dense_replace,
                dense_intermediate_size=hf_config.intermediate_size
            )
        else:
            dense_replace = DenseReplaceConfig(
                first_k_moe_replace_by_dense=0,
                dense_intermediate_size=0)

        if dtype == 'auto':
            dtype = getattr(hf_config, 'torch_dtype', None)
            if dtype is None:
                dtype = 'float16'
            if isinstance(dtype, torch.dtype):
                dtype = torch_dtype_to_str(dtype)
            if dtype == 'float32':
                dtype = 'float16'
        if dtype == 'bfloat16' and torch.cuda.get_device_properties(
                0).major < 8:
            logger.warning(
                "Pre SM 80 GPUs do not support bfloat16, fallback to float16")
            dtype = 'float16'

        return cls(
            architecture='LlamaForCausalLM',
            dtype=dtype,
            num_hidden_layers=hf_config.num_hidden_layers,
            num_attention_heads=hf_config.num_attention_heads,
            hidden_size=hf_config.hidden_size,
            intermediate_size=intermediate_size,
            num_key_value_heads=num_key_value_heads,
            vocab_size=hf_config.vocab_size,
            position_embedding_type='rope_gpt_neox',
            max_position_embeddings=hf_config.max_position_embeddings,
            hidden_act=hidden_act,
            norm_epsilon=hf_config.rms_norm_eps,
            attn_bias=attn_bias,
            rotary_base=rotary_base,
            rotary_scaling=rotary_scaling,
            residual_mlp=residual_mlp,
            disable_weight_only_quant_plugin=disable_weight_only_quant_plugin,
            moe=moe_config,
            mapping=mapping,
            quantization=quant_config,
            dense_replace=dense_replace,
            **kwargs)

    @classmethod
    def from_meta_ckpt(cls,
                       meta_ckpt_dir: str,
                       dtype: str = 'auto',
                       mapping: Optional[Mapping] = None,
                       quant_config: Optional[QuantConfig] = None,
                       **kwargs):

        with open(Path(meta_ckpt_dir, "params.json")) as fp:
            meta_config: dict = json.load(fp)

        n_embd = meta_config["dim"]
        n_head = meta_config["n_heads"]
        n_kv_head = meta_config.get("n_kv_heads", n_head)
        vocab_size = meta_config.get("vocab_size", 32000)

        # Reset vocab_size to 32000 for LLama v2 checkpoint.
        if vocab_size == -1:
            vocab_size = 32000

        if "hidden_dim" in meta_config:
            inter_size = meta_config["hidden_dim"]
        else:
            multiple_of = meta_config.get("multiple_of", 1)
            n_embd_ = int(4 * n_embd * 2 / 3)
            ffn_dim_multiplier = meta_config.get("ffn_dim_multiplier", 1)
            inter_size = multiple_of * (
                (int(n_embd_ * ffn_dim_multiplier) + multiple_of - 1) //
                multiple_of)

        if dtype == 'auto':
            dtype = 'bfloat16'
        if dtype == 'bfloat16' and torch.cuda.get_device_properties(
                0).major < 8:
            logger.warning(
                "Pre SM 80 GPUs do not support bfloat16, fallback to float16")
            dtype = 'float16'

        # meta checkpoint don't have vocab_size|hidden_act|rotary_base specified, use same default value as HF
        return cls(architecture="LlamaForCausalLM",
                   dtype=dtype,
                   num_hidden_layers=meta_config["n_layers"],
                   num_attention_heads=n_head,
                   hidden_size=n_embd,
                   intermediate_size=inter_size,
                   num_key_value_heads=n_kv_head,
                   vocab_size=vocab_size,
                   position_embedding_type='rope_gpt_neox',
                   max_position_embeddings=2048,
                   hidden_act='silu',
                   rotary_base=meta_config.get('rope_theta', 10000),
                   norm_epsilon=meta_config["norm_eps"],
                   mapping=mapping,
                   quantization=quant_config,
                   **kwargs)
