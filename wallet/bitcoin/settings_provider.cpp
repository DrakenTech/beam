// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "settings_provider.h"

namespace beam::bitcoin
{
    SettingsProvider::SettingsProvider(wallet::IWalletDB::Ptr walletDB)
        : m_walletDB(walletDB)
    {
    }

    BitcoinCoreSettings SettingsProvider::GetBitcoinCoreSettings() const
    {
        assert(m_settings);
        return m_settings->GetConnectionOptions();
    }

    ElectrumSettings SettingsProvider::GetElectrumSettings() const
    {
        assert(m_settings);
        return m_settings->GetElectrumConnectionOptions();
    }

    Settings SettingsProvider::GetSettings() const
    {
        assert(m_settings);
        return *m_settings;
    }

    void SettingsProvider::SetSettings(const Settings& settings)
    {
        // store to DB
        {
            auto coreSettings = settings.GetConnectionOptions();

            WriteToDb(GetUserName(), coreSettings.m_userName);
            WriteToDb(GetPassName(), coreSettings.m_pass);
            WriteToDb(GetAddressName(), coreSettings.m_address);
        }

        {
            auto electrumSettings = settings.GetElectrumConnectionOptions();

            WriteToDb(GetElectrumAddressName(), electrumSettings.m_address);
            WriteToDb(GetSecretWordsName(), electrumSettings.m_secretWords);
            WriteToDb(GetAddressVersionName(), electrumSettings.m_addressVersion);
        }

        WriteToDb(GetFeeRateName(), settings.GetFeeRate());
        WriteToDb(GetMinFeeRateName(), settings.GetMinFeeRate());
        WriteToDb(GetTxMinConfirmationsName(), settings.GetTxMinConfirmations());
        WriteToDb(GetLockTimeInBlocksName(), settings.GetLockTimeInBlocks());
        WriteToDb(GetConnectrionTypeName(), settings.GetCurrentConnectionType());

        // update m_settings
        m_settings = std::make_unique<Settings>(settings);
    }

    void SettingsProvider::ResetSettings()
    {
        // remove from DB
        m_walletDB->removeVarRaw(GetSettingsName().c_str());

        m_settings = std::make_unique<Settings>(GetEmptySettings());
    }

    void SettingsProvider::Initialize()
    {
        if (!m_settings)
        {
            m_settings = std::make_unique<Settings>(GetEmptySettings());

            {
                BitcoinCoreSettings settings;

                ReadFromDB(GetUserName(), settings.m_userName);
                ReadFromDB(GetPassName(), settings.m_pass);
                ReadFromDB(GetAddressName(), settings.m_address);

                m_settings->SetConnectionOptions(settings);
            }
            {
                ElectrumSettings settings;

                ReadFromDB(GetElectrumAddressName(), settings.m_address);
                ReadFromDB(GetSecretWordsName(), settings.m_secretWords);
                ReadFromDB(GetAddressVersionName(), settings.m_addressVersion);

                m_settings->SetElectrumConnectionOptions(settings);
            }

            {
                auto feeRate = m_settings->GetFeeRate();
                ReadFromDB(GetFeeRateName(), feeRate);
                m_settings->SetFeeRate(feeRate);
            }

            {
                auto minFeeRate = m_settings->GetMinFeeRate();
                ReadFromDB(GetMinFeeRateName(), minFeeRate);
                m_settings->SetMinFeeRate(minFeeRate);
            }

            {
                auto txMinConfirmations = m_settings->GetTxMinConfirmations();
                ReadFromDB(GetTxMinConfirmationsName(), txMinConfirmations);
                m_settings->SetTxMinConfirmations(txMinConfirmations);
            }

            {
                auto lockTimeInBlocks = m_settings->GetLockTimeInBlocks();
                ReadFromDB(GetLockTimeInBlocksName(), lockTimeInBlocks);
                m_settings->SetLockTimeInBlocks(lockTimeInBlocks);
            }

            {
                auto connectionType = m_settings->GetCurrentConnectionType();
                ReadFromDB(GetConnectrionTypeName(), connectionType);
                m_settings->ChangeConnectionType(connectionType);
            }
        }
    }

    bool SettingsProvider::CanModify() const
    {
        return m_refCount == 0;
    }

    void SettingsProvider::AddRef()
    {
        ++m_refCount;
    }

    void SettingsProvider::ReleaseRef()
    {
        if (m_refCount)
        {
            --m_refCount;
        }
    }

    std::string SettingsProvider::GetSettingsName() const
    {
        return "BTCSettings";
    }

    Settings SettingsProvider::GetEmptySettings()
    {
        return Settings{};
    }

    std::string SettingsProvider::GetUserName() const
    {
        return GetSettingsName() + "_UserName";
    }

    std::string SettingsProvider::GetPassName() const
    {
        return GetSettingsName() + "_Pass";
    }

    std::string SettingsProvider::GetAddressName() const
    {
        return GetSettingsName() + "_Address";
    }

    std::string SettingsProvider::GetElectrumAddressName() const
    {
        return GetSettingsName() + "_ElectrumAddress";
    }

    std::string SettingsProvider::GetSecretWordsName() const
    {
        return GetSettingsName() + "_SecretWords";
    }

    std::string SettingsProvider::GetAddressVersionName() const
    {
        return GetSettingsName() + "_AddressVersion";
    }

    std::string SettingsProvider::GetFeeRateName() const
    {
        return GetSettingsName() + "_FeeRate";
    }

    std::string SettingsProvider::GetMinFeeRateName() const
    {
        return GetSettingsName() + "_MinFeeRate";
    }

    std::string SettingsProvider::GetTxMinConfirmationsName() const
    {
        return GetSettingsName() + "_TxMinConfirmations";
    }

    std::string SettingsProvider::GetLockTimeInBlocksName() const
    {
        return GetSettingsName() + "_LockTimeInBlocks";
    }

    std::string SettingsProvider::GetConnectrionTypeName() const
    {
        return GetSettingsName() + "_ConnectionType";
    }
} // namespace beam::bitcoin