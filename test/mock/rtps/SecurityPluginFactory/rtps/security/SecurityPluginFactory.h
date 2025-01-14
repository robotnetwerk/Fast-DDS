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
 * @file SecurityPluginFactory.h
 */
#ifndef _RTPS_SECURITY_SECURITYPLUGINFACTORY_H_
#define _RTPS_SECURITY_SECURITYPLUGINFACTORY_H_

#include <fastdds/rtps/attributes/PropertyPolicy.h>
#include <fastdds/rtps/security/accesscontrol/AccessControl.h>
#include <fastdds/rtps/security/authentication/Authentication.h>
#include <fastdds/rtps/security/cryptography/Cryptography.h>
#include <fastdds/rtps/security/logging/Logging.h>

#include <rtps/security/ISecurityPluginFactory.h>

namespace eprosima {
namespace fastrtps {
namespace rtps {
namespace security {

class SecurityPluginFactory : public ISecurityPluginFactory
{
public:

    Authentication* create_authentication_plugin(
            const PropertyPolicy& property_policy) override;

    AccessControl* create_access_control_plugin(
            const PropertyPolicy& property_policy) override;

    Cryptography* create_cryptography_plugin(
            const PropertyPolicy& property_policy) override;

    Logging* create_logging_plugin(
            const PropertyPolicy& property_policy) override;

    static void set_auth_plugin(
            Authentication* plugin);

    static void release_auth_plugin();

    static void set_access_control_plugin(
            AccessControl* plugin);

    static void release_access_control_plugin();

    static void set_crypto_plugin(
            Cryptography* plugin);

    static void release_crypto_plugin();

    static void set_logging_plugin(
            Logging* plugin);

    static void release_logging_plugin();

private:

    static Authentication* auth_plugin_;

    static AccessControl* access_plugin_;

    static Cryptography* crypto_plugin_;

    static Logging* logging_plugin_;
};

} //namespace security
} //namespace rtps
} //namespace fastrtps
} //namespace eprosima

#endif // _RTPS_SECURITY_SECURITYPLUGINFACTORY_H_
