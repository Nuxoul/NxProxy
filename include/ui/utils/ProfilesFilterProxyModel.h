#pragma once

#include <QSortFilterProxyModel>
#include <QSet>
#include <QString>

class ProfilesTableModel;

class ProfilesFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ProfilesFilterProxyModel(QObject *parent = nullptr);

    // `address` also takes "port=N", "port=MIN:MAX", "port=MIN:", "port=:MAX".
    void setFilters(const QString &type, const QString &address, const QString &name, const QString &country);
    void setAllowedProfileIds(const QList<int> &ids);
    void clearAllowedProfileIds();
    bool hasActiveFilter() const;

    ProfilesTableModel *profilesModel() const;
    int toSourceRow(int proxyRow) const;
    int toProxyRow(int sourceRow) const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool portMatches(int port) const;

    bool m_allowedProfileIdsEnabled = false;
    QSet<int> m_allowedProfileIds;
    QString m_type;
    QString m_address;
    QString m_name;
    QString m_country;
};
