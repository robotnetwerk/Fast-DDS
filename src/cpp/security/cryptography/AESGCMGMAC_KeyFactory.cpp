// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*!
 * @file AESGCMGMAC_KeyFactory.cpp
 */

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>

#include "AESGCMGMAC_KeyFactory.h"

using namespace eprosima::fastrtps::rtps::security;

AESGCMGMAC_KeyFactory::AESGCMGMAC_KeyFactory(){}

ParticipantCryptoHandle * AESGCMGMAC_KeyFactory::register_local_participant(
                const IdentityHandle &participant_identity, 
                const PermissionsHandle &participant_permissions, 
                const PropertySeq &participant_properties, 
                SecurityException &exception){

    AESGCMGMAC_ParticipantCryptoHandle* PCrypto;
    if( (!participant_identity.nil()) | (!participant_permissions.nil()) ){
        exception = SecurityException("Invalid input parameters");
        return nullptr;
    }

    PCrypto = new AESGCMGMAC_ParticipantCryptoHandle();
    (*PCrypto)->KeyMaterial = create_KeyMaterial();
    if((*PCrypto)->KeyMaterial == nullptr){
        exception = SecurityException("Unable to create Crypto material");
        return nullptr;
    }else{
        return PCrypto;
    }
    exception = SecurityException("Not implemented");
    return nullptr;
}
        
ParticipantCryptoHandle * AESGCMGMAC_KeyFactory::register_matched_remote_participant(
                ParticipantCryptoHandle &local_participant_crypto_handle, 
                IdentityHandle &remote_participant_identity, 
                PermissionsHandle &remote_participant_permissions, 
                SharedSecretHandle &shared_secret, 
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return nullptr;
}

        DatawriterCryptoHandle * AESGCMGMAC_KeyFactory::register_local_datawriter(
                const ParticipantCryptoHandle &participant_crypto,
                const PropertySeq &datawriter_prop,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return nullptr;
}

        DatareaderCryptoHandle * AESGCMGMAC_KeyFactory::register_matched_remote_datareader(
                const DatawriterCryptoHandle &local_datawriter_crypto_handle,
                const ParticipantCryptoHandle &lremote_participant_crypto,
                const SharedSecretHandle &shared_secret,
                const bool relay_only,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return nullptr;
}

        DatareaderCryptoHandle * AESGCMGMAC_KeyFactory::register_local_datareader(
                const ParticipantCryptoHandle &participant_crypto,
                const PropertySeq &datareader_properties,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return nullptr;
}

        DatawriterCryptoHandle * AESGCMGMAC_KeyFactory::register_matched_remote_datawriter(
                const DatareaderCryptoHandle &local_datareader_crypto_handle,
                const ParticipantCryptoHandle &remote_participant_crypt,
                const SharedSecretHandle &shared_secret,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return nullptr;
}

        bool AESGCMGMAC_KeyFactory::unregister_participant(
                const ParticipantCryptoHandle &participant_crypto_handle,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return false;
}
        
        bool AESGCMGMAC_KeyFactory::unregister_datawriter(
                const DatawriterCryptoHandle &datawriter_crypto_handle,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return false;
}

        
        bool AESGCMGMAC_KeyFactory::unregister_datareader(
                const DatareaderCryptoHandle &datareader_crypto_handle,
                SecurityException &exception){

    exception = SecurityException("Not implemented");
    return false;
}


KeyMaterial_AES_GCM_GMAC * AESGCMGMAC_KeyFactory::create_KeyMaterial(){



    return nullptr;
}
