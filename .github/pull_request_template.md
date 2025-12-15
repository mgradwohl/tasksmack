## Description

<!-- Describe your changes in detail -->

## Type of Change

<!-- Mark the relevant option with an [x] -->

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Documentation update
- [ ] Build/CI improvement

## Checklist

<!-- Mark completed items with an [x] -->

- [ ] I have read the [CONTRIBUTING](CONTRIBUTING.md) guidelines
- [ ] My code follows the project's coding standards
- [ ] I have run `clang-format` on my changes
- [ ] I have run `clang-tidy` and addressed any warnings
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
