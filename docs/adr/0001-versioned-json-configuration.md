# ADR-0001: Versioned JSON configuration with atomic Windows replacement

- Status: Accepted
- Date: 2026-08-16

## Context

Desto 的配置需要支持格式演进、存储根迁移和未知字段保留。关键写入不能因为进程退出或系统重启而留下半个文件；同时 Storage 不应依赖 UI 框架。

## Options

- 手写 JSON 解析：没有第三方源码成本，但转义、类型错误和未来兼容性容易形成长期维护风险。
- 引入重量级配置框架：功能完整，但发布体积和运行时依赖超出当前轻量目标。
- 使用单头文件 `nlohmann/json`：结构化解析能力完整，运行时无额外服务依赖，代价是编译时间和约 1 MB 源码。

## Decision

使用 vendored `nlohmann/json` 3.11.3，仅由 `JsonConfigStore` 使用。配置根对象包含 `schemaVersion`，当前 schema 由 `ApplicationConfig::CurrentSchemaVersion` 唯一声明（现为 27），存储根位于 `storage.root`；保存时读取现有文档并只更新已知字段，以保留未知字段。迁移链逐版本执行且每一步可测试；最初的 v1 到 v4 补齐 Card 集合、实例内容偏好和稀疏自定义槽位，后续版本演进记录在 `docs/CONFIGURATION.md`。

Windows 写入流程为临时文件 `CREATE_NEW`、`FlushFileBuffers`、`ReplaceFileW`（首次写入使用 `MoveFileExW`）。替换前复制最后有效文件为 `.bak`；临时文件发布失败时删除，不覆盖最后一次有效配置。主文件损坏时只恢复 `.bak`，未来 schema 不回退。

## Consequences

- 配置结构可验证、可迁移，旧版本不会主动抹掉新字段。
- 常驻运行时不增加 JSON 服务或动态库；编译依赖和源码体积增加。
- `JsonConfigStore` 暂不负责业务迁移编排，存储根迁移由 `StorageRootMigrationService` 执行，配置保存失败时由调用方用返回的移动清单回滚。

## Validation

- `desto_storage_tests` 验证未知字段保留、Windows 原子写入后无临时文件残留。
- `desto_storage_tests` 和 `desto_domain_tests` 在 Debug 配置下均通过。
