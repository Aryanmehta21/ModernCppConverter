from typing import Literal

from pydantic import BaseModel, Field, field_validator


class ConversionChangeModel(BaseModel):
    ruleName: str
    before: str = ""
    after: str = ""
    reason: str = ""
    applied: bool = False
    skipped: bool = False


class ConversionResultModel(BaseModel):
    modernCode: str = ""
    changes: list[ConversionChangeModel] = Field(default_factory=list)
    explanation: str = ""


class ModernizationOptionsModel(BaseModel):
    useNullptr: bool = True
    useUsingAliases: bool = True
    useSmartPointers: bool = True
    useMakeUnique: bool = True
    applySafeOwnershipModernization: bool = True
    useStringView: bool = True
    applyStringViewWhenSafe: bool = False
    customInstruction: str = ""


class ConvertRequest(BaseModel):
    code: str
    mode: Literal["offline", "online", "hybrid"] = "online"
    targetStandard: Literal["C++17", "C++20"] = "C++20"
    options: ModernizationOptionsModel = Field(default_factory=ModernizationOptionsModel)
    localResult: ConversionResultModel | None = None

    @field_validator("code")
    @classmethod
    def code_must_not_be_empty(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("legacyCode must not be empty")
        return value


class ConvertResponse(BaseModel):
    ok: bool = True
    modernCode: str = ""
    changes: list[ConversionChangeModel] = Field(default_factory=list)
    suggestions: list[str] = Field(default_factory=list)
    explanation: str = ""
    warnings: list[str] = Field(default_factory=list)
    confidence: float = 0.0
    error: str = ""
