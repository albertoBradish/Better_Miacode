#include "app/ui/document/DocumentModel.h"
#include "app/services/ChartFileSafety.h"
#include "core/chart/document/AdvancedMetadata.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

namespace miacode::ui {

QString DocumentModel::metadataConflictText() const
{
    if (currentFilePath().isEmpty()) return {};
    QFile file(currentFilePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto text = miacode::ChartWorkspaceFileService::decodeDocumentText(file.readAll());
    const auto conflicts = text ? miacode::metadata::conflictingKeys(*text) : QStringList{};
    return conflicts.isEmpty() ? QString() : qtTrId("metadata.advanced.conflict_warning").arg(conflicts.join(QStringLiteral(", ")));
}

QString DocumentModel::advancedMetadataValue(const QString& key) const
{
    return workspace_ ? miacode::metadata::fieldValue(workspace_->document(), key) : QString();
}

QVariantList DocumentModel::advancedMetadataGroups(int difficultyId) const
{
    QVariantList groups;
    if (!workspace_ || !hasDocument()) return groups;
    for (const auto& field : miacode::metadata::advancedFields(difficultyId)) {
        const QString groupKey = QString::fromLatin1(field.group);
        int index = -1;
        for (int i = 0; i < groups.size(); ++i)
            if (groups.at(i).toMap().value(QStringLiteral("id")).toString() == groupKey) { index = i; break; }
        if (index < 0) {
            index = groups.size();
            groups.append(QVariantMap{{QStringLiteral("id"), groupKey}, {QStringLiteral("title"), qtTrId(field.group)}, {QStringLiteral("titleKey"), groupKey},
                                     {QStringLiteral("fields"), QVariantList{}}});
        }
        auto group = groups.at(index).toMap();
        auto fields = group.value(QStringLiteral("fields")).toList();
        fields.append(QVariantMap{{QStringLiteral("key"), field.key}, {QStringLiteral("label"), qtTrId(field.label)},
            {QStringLiteral("labelKey"), QString::fromLatin1(field.label)},
            {QStringLiteral("value"), miacode::metadata::fieldValue(workspace_->document(), field.key)},
            {QStringLiteral("multiline"), field.multiline},
            {QStringLiteral("helpKey"), QString::fromLatin1(field.help)},
            {QStringLiteral("example"), field.example}, {QStringLiteral("unit"), field.unit},
            {QStringLiteral("suggestions"), field.suggestions}, {QStringLiteral("readOnly"), field.readOnly}});
        group.insert(QStringLiteral("fields"), fields);
        groups[index] = group;
    }
    return groups;
}

QString DocumentModel::setAdvancedMetadataField(const QString& key, const QString& value)
{
    if (!workspace_ || !hasDocument()) return qtTrId("metadata.advanced.no_document");
    bool allowed = false;
    for (int slot = 1; slot <= 7 && !allowed; ++slot)
        for (const auto& field : miacode::metadata::advancedFields(slot)) if (field.key == key) {
            if (field.readOnly) return qtTrId("metadata.reference.managed_field");
            allowed = true;
            break;
        }
    if (!allowed) return qtTrId("metadata.advanced.unknown_field");
    const QString conflicts = metadataConflictText();
    if (!conflicts.isEmpty()) return conflicts;
    const QString validation = miacode::metadata::validateField(key, value);
    if (!validation.isEmpty()) return validation;
    if (miacode::metadata::fieldValue(workspace_->document(), key) == value) return {};
    // Preserve the original before an advanced form can normalize legacy metadata.
    if (!currentFilePath().isEmpty() && QFileInfo::exists(currentFilePath())) {
        QString error;
        if (!miacode::chart_file_safety::backupOriginal(currentFilePath(), nullptr, &error)) return error;
    }
    QVector<SimaiRawField> fields;
    for (const auto& field : workspace_->document().extraFields)
        if (field.key.compare(key, Qt::CaseInsensitive) != 0) fields.append(field);
    if (!value.trimmed().isEmpty()) fields.append({key, value});
    // An explicit demo edit makes the shared preview window user-owned.
    if (key == QLatin1String("demo_seek") || key == QLatin1String("demo_len")) {
        for (qsizetype i = fields.size(); i-- > 0;)
            if (fields.at(i).key == QLatin1String("miacode_demo_source")) fields.removeAt(i);
        fields.append({QStringLiteral("miacode_demo_source"), QStringLiteral("manual")});
    }
    bool accepted = false;
    runWorkspaceMutation([&] {
        accepted = workspace_->replaceExtraFields(fields).accepted;
        return accepted;
    });
    if (!accepted) return qtTrId("metadata.advanced.update_failed");
    publishWorkspaceCommit(WorkspaceCommitKind::Incremental);
    return {};
}

} // namespace miacode::ui
