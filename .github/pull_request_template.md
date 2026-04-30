## Description

<!-- Describe your changes in detail -->

## PR Size

<!-- Aim for small, focused PRs. Guidelines:
- ✅ Small (< 200 lines changed): ideal, fast to review
- ⚠️  Medium (200–500 lines): acceptable with clear description
- 🔴 Large (> 500 lines): split into smaller PRs if possible
Exclude generated files, vendored code, and lock files from the count. -->

## Type of Change

<!-- Mark the relevant option with an [x] -->

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Documentation update
- [ ] Build/CI improvement

## Checklist

<!-- Mark completed items with an [x] -->

- [ ] I have read the [CONTRIBUTING](../CONTRIBUTING.md) guidelines
- [ ] My code follows the project's coding standards
- [ ] I have run `clang-tidy` and addressed any warnings
- [ ] I have run `clang-format` on my changes
- [ ] I have run `pre-commit run --all-files` or installed pre-commit hooks
- [ ] I have added tests that prove my fix/feature works
- [ ] All new and existing tests pass
- [ ] I have updated documentation as needed

## Testing

<!-- Describe how you tested your changes -->

```bash
# Commands used to test
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Additional Notes

<!-- Any additional information that reviewers should know -->
