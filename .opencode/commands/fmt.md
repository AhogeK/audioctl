---
description: "强制执行 LLVM 风格的代码格式化"
---

# 🎨 格式化执行流

提交任何代码至暂存区前，请执行此命令确保符合 `.clang-format` 契约：

```bash
find src include tests -name "*.[chm]" | xargs clang-format -i
```