/*
 *
 *    Copyright (c) 2020-2022 Project CHIP Authors
 *    Copyright (c) 2013-2017 Nest Labs, Inc.
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements methods for converting a CHIP
 *      TLV-encoded certificate to a standard X.509 certificate.
 *
 */

#include <inttypes.h>
#include <stddef.h>

#include <credentials/CHIPCert_Internal.h>
#include <lib/asn1/ASN1.h>
#include <lib/asn1/ASN1Macros.h>
#include <lib/core/CHIPCore.h>
#include <lib/core/CHIPSafeCasts.h>
#include <lib/core/TLV.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/DLLUtil.h>
#include <lib/support/SafeInt.h>
#include <protocols/Protocols.h>

namespace chip {
namespace Credentials {

using namespace chip::ASN1;
using namespace chip::TLV;
using namespace chip::Protocols;
using namespace chip::Crypto;

static CHIP_ERROR DecodeConvertDN(TLVReader & reader, ASN1Writer & writer, ChipDN & dn)
{
    ReturnErrorOnFailure(dn.DecodeFromTLV(reader));
    ReturnErrorOnFailure(dn.EncodeToASN1(writer));
    return CHIP_NO_ERROR;
}

static CHIP_ERROR DecodeConvertValidity(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    ASN1UniversalTime asn1Time;

    ASN1_START_SEQUENCE
    {
        ReturnErrorOnFailure(reader.Next(ContextTag(kTag_NotBefore)));
        ReturnErrorOnFailure(reader.Get(certData.mNotBeforeTime));
        ReturnErrorOnFailure(ChipEpochToASN1Time(certData.mNotBeforeTime, asn1Time));
        ASN1_ENCODE_TIME(asn1Time);

        ReturnErrorOnFailure(reader.Next(ContextTag(kTag_NotAfter)));
        ReturnErrorOnFailure(reader.Get(certData.mNotAfterTime));
        ReturnErrorOnFailure(ChipEpochToASN1Time(certData.mNotAfterTime, asn1Time));
        ASN1_ENCODE_TIME(asn1Time);

        // Perform this check if NotAfter value is different from Never-Expire value.
        if (certData.mNotAfterTime != kNullCertTime)
        {
            VerifyOrReturnError(certData.mNotBeforeTime < certData.mNotAfterTime, CHIP_ERROR_UNSUPPORTED_CERT_FORMAT);
        }
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

static CHIP_ERROR DecodeConvertSubjectPublicKeyInfo(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    uint8_t pubKeyAlgoId, pubKeyCurveId;

    ReturnErrorOnFailure(reader.Next(ContextTag(kTag_PublicKeyAlgorithm)));
    ReturnErrorOnFailure(reader.Get(pubKeyAlgoId));

    certData.mPubKeyAlgoOID = GetOID(kOIDCategory_PubKeyAlgo, pubKeyAlgoId);
    VerifyOrReturnError(certData.mPubKeyAlgoOID == kOID_PubKeyAlgo_ECPublicKey, CHIP_ERROR_UNSUPPORTED_CERT_FORMAT);

    ReturnErrorOnFailure(reader.Next(ContextTag(kTag_EllipticCurveIdentifier)));
    ReturnErrorOnFailure(reader.Get(pubKeyCurveId));

    certData.mPubKeyCurveOID = GetOID(kOIDCategory_EllipticCurve, pubKeyCurveId);
    VerifyOrReturnError(certData.mPubKeyCurveOID == kOID_EllipticCurve_prime256v1, CHIP_ERROR_UNSUPPORTED_ELLIPTIC_CURVE);

    // subjectPublicKeyInfo SubjectPublicKeyInfo,
    ASN1_START_SEQUENCE
    {
        // algorithm AlgorithmIdentifier,
        // AlgorithmIdentifier ::= SEQUENCE
        ASN1_START_SEQUENCE
        {
            // algorithm OBJECT IDENTIFIER,
            ASN1_ENCODE_OBJECT_ID(certData.mPubKeyAlgoOID);

            // EcpkParameters ::= CHOICE {
            //     ecParameters  ECParameters,
            //     namedCurve    OBJECT IDENTIFIER,
            //     implicitlyCA  NULL }
            //
            // (Only namedCurve supported).
            //
            ASN1_ENCODE_OBJECT_ID(certData.mPubKeyCurveOID);
        }
        ASN1_END_SEQUENCE;

        ReturnErrorOnFailure(reader.Next(kTLVType_ByteString, ContextTag(kTag_EllipticCurvePublicKey)));
        ReturnErrorOnFailure(reader.Get(certData.mPublicKey));

        static_assert(P256PublicKeySpan().size() <= UINT16_MAX, "Public key size doesn't fit in a uint16_t");

        // For EC certs, the subjectPublicKey BIT STRING contains the X9.62 encoded EC point.
        ReturnErrorOnFailure(writer.PutBitString(0, certData.mPublicKey.data(), static_cast<uint16_t>(certData.mPublicKey.size())));
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

static CHIP_ERROR DecodeConvertAuthorityKeyIdentifierExtension(TLVReader & reader, ASN1Writer & writer,
                                                               ChipCertificateData & certData)
{
    CHIP_ERROR err;

    certData.mCertFlags.Set(CertFlags::kExtPresent_AuthKeyId);

    // AuthorityKeyIdentifier extension MUST be marked as non-critical (default).

    // AuthorityKeyIdentifier ::= SEQUENCE
    ASN1_START_SEQUENCE
    {
        // keyIdentifier [0] IMPLICIT KeyIdentifier
        // KeyIdentifier ::= OCTET STRING
        ReturnErrorOnFailure(reader.Expect(kTLVType_ByteString, ContextTag(kTag_AuthorityKeyIdentifier)));
        ReturnErrorOnFailure(reader.Get(certData.mAuthKeyId));

        static_assert(CertificateKeyId().size() <= UINT16_MAX, "Authority key id size doesn't fit in a uint16_t");

        ReturnErrorOnFailure(writer.PutOctetString(kASN1TagClass_ContextSpecific, 0, certData.mAuthKeyId.data(),
                                                   static_cast<uint16_t>(certData.mAuthKeyId.size())));
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

static CHIP_ERROR DecodeConvertSubjectKeyIdentifierExtension(TLVReader & reader, ASN1Writer & writer,
                                                             ChipCertificateData & certData)
{
    certData.mCertFlags.Set(CertFlags::kExtPresent_SubjectKeyId);

    // SubjectKeyIdentifier extension MUST be marked as non-critical (default).

    // SubjectKeyIdentifier ::= KeyIdentifier
    // KeyIdentifier ::= OCTET STRING
    ReturnErrorOnFailure(reader.Expect(kTLVType_ByteString, ContextTag(kTag_SubjectKeyIdentifier)));
    ReturnErrorOnFailure(reader.Get(certData.mSubjectKeyId));

    static_assert(CertificateKeyId().size() <= UINT16_MAX, "Subject key id size doesn't fit in a uint16_t");

    ReturnErrorOnFailure(
        writer.PutOctetString(certData.mSubjectKeyId.data(), static_cast<uint16_t>(certData.mSubjectKeyId.size())));

    return CHIP_NO_ERROR;
}

static CHIP_ERROR DecodeConvertKeyUsageExtension(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    uint16_t keyUsageBits;

    certData.mCertFlags.Set(CertFlags::kExtPresent_KeyUsage);

    // KeyUsage ::= BIT STRING
    ReturnErrorOnFailure(reader.Expect(ContextTag(kTag_KeyUsage)));
    ReturnErrorOnFailure(reader.Get(keyUsageBits));

    {
        BitFlags<KeyUsageFlags> keyUsageFlags(keyUsageBits);
        VerifyOrReturnError(
            keyUsageFlags.HasOnly(KeyUsageFlags::kDigitalSignature, KeyUsageFlags::kNonRepudiation, KeyUsageFlags::kKeyEncipherment,
                                  KeyUsageFlags::kDataEncipherment, KeyUsageFlags::kKeyAgreement, KeyUsageFlags::kKeyCertSign,
                                  KeyUsageFlags::kCRLSign, KeyUsageFlags::kEncipherOnly, KeyUsageFlags::kEncipherOnly),
            CHIP_ERROR_UNSUPPORTED_CERT_FORMAT);

        ASN1_ENCODE_BIT_STRING(keyUsageBits);

        certData.mKeyUsageFlags = keyUsageFlags;
    }

exit:
    return err;
}

static CHIP_ERROR DecodeConvertBasicConstraintsExtension(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    TLVType outerContainer;

    certData.mCertFlags.Set(CertFlags::kExtPresent_BasicConstraints);

    // BasicConstraints ::= SEQUENCE
    ASN1_START_SEQUENCE
    {
        ReturnErrorOnFailure(reader.Expect(kTLVType_Structure, ContextTag(kTag_BasicConstraints)));
        ReturnErrorOnFailure(reader.EnterContainer(outerContainer));

        // cA BOOLEAN DEFAULT FALSE
        {
            bool isCA;
            ReturnErrorOnFailure(reader.Next(ContextTag(kTag_BasicConstraints_IsCA)));
            ReturnErrorOnFailure(reader.Get(isCA));

            if (isCA)
            {
                ASN1_ENCODE_BOOLEAN(true);
                certData.mCertFlags.Set(CertFlags::kIsCA);
            }

            err = reader.Next();
            VerifyOrReturnError(err == CHIP_NO_ERROR || err == CHIP_END_OF_TLV, err);
        }

        // pathLenConstraint INTEGER (0..MAX) OPTIONAL
        if (reader.GetTag() == ContextTag(kTag_BasicConstraints_PathLenConstraint))
        {
            ReturnErrorOnFailure(reader.Get(certData.mPathLenConstraint));

            ASN1_ENCODE_INTEGER(certData.mPathLenConstraint);

            certData.mCertFlags.Set(CertFlags::kPathLenConstraintPresent);

            err = reader.Next();
            VerifyOrReturnError(err == CHIP_END_OF_TLV, err);
        }

        ReturnErrorOnFailure(reader.VerifyEndOfContainer());
        ReturnErrorOnFailure(reader.ExitContainer(outerContainer));
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

static CHIP_ERROR DecodeConvertExtendedKeyUsageExtension(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    TLVType outerContainer;

    certData.mCertFlags.Set(CertFlags::kExtPresent_ExtendedKeyUsage);

    // ExtKeyUsageSyntax ::= SEQUENCE SIZE (1..MAX) OF KeyPurposeId
    ASN1_START_SEQUENCE
    {
        ReturnErrorOnFailure(reader.Expect(kTLVType_Array, ContextTag(kTag_ExtendedKeyUsage)));
        ReturnErrorOnFailure(reader.EnterContainer(outerContainer));

        while ((err = reader.Next(AnonymousTag())) == CHIP_NO_ERROR)
        {
            uint8_t keyPurposeId;
            ReturnErrorOnFailure(reader.Get(keyPurposeId));

            // KeyPurposeId ::= OBJECT IDENTIFIER
            ASN1_ENCODE_OBJECT_ID(GetOID(kOIDCategory_KeyPurpose, keyPurposeId));

            certData.mKeyPurposeFlags.Set(static_cast<KeyPurposeFlags>(0x01 << (keyPurposeId - 1)));
        }
        VerifyOrReturnError(err == CHIP_END_OF_TLV, err);
        ReturnErrorOnFailure(reader.ExitContainer(outerContainer));
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

static CHIP_ERROR DecodeConvertFutureExtension(TLVReader & tlvReader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    ByteSpan extensionSequence;
    ASN1Reader reader;

    ReturnErrorOnFailure(tlvReader.Expect(kTLVType_ByteString, ContextTag(kTag_FutureExtension)));
    ReturnErrorOnFailure(tlvReader.Get(extensionSequence));

    reader.Init(extensionSequence);

    // Extension ::= SEQUENCE
    ASN1_PARSE_ENTER_SEQUENCE
    {
        OID extensionOID;
        bool critical = false;

        ASN1_PARSE_OBJECT_ID(extensionOID);

        VerifyOrReturnError(extensionOID == kOID_Unknown, ASN1_ERROR_UNSUPPORTED_ENCODING);

        // critical BOOLEAN DEFAULT FALSE,
        ASN1_PARSE_ANY;
        if (reader.GetClass() == kASN1TagClass_Universal && reader.GetTag() == kASN1UniversalTag_Boolean)
        {
            ASN1_GET_BOOLEAN(critical);

            if (critical)
            {
                certData.mCertFlags.Set(CertFlags::kExtPresent_FutureIsCritical);
            }

            ASN1_PARSE_ANY;
        }
    }
    ASN1_EXIT_SEQUENCE;

    VerifyOrReturnError(CanCastTo<uint16_t>(extensionSequence.size()), ASN1_ERROR_INVALID_ENCODING);

    // FutureExtension SEQUENCE
    ReturnErrorOnFailure(writer.PutConstructedType(extensionSequence.data(), static_cast<uint16_t>(extensionSequence.size())));

exit:
    return err;
}

static CHIP_ERROR DecodeConvertExtension(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    Tag tlvTag;
    uint32_t extensionTagNum;

    tlvTag = reader.GetTag();
    VerifyOrReturnError(IsContextTag(tlvTag), CHIP_ERROR_INVALID_TLV_TAG);
    extensionTagNum = TagNumFromTag(tlvTag);

    if (extensionTagNum == kTag_FutureExtension)
    {
        ReturnErrorOnFailure(DecodeConvertFutureExtension(reader, writer, certData));
    }
    else
    {
        // Extension ::= SEQUENCE
        ASN1_START_SEQUENCE
        {
            // extnID OBJECT IDENTIFIER,
            ASN1_ENCODE_OBJECT_ID(GetOID(kOIDCategory_Extension, static_cast<uint8_t>(extensionTagNum)));

            // BasicConstraints, KeyUsage and ExtKeyUsage extensions MUST be marked as critical.
            if (extensionTagNum == kTag_KeyUsage || extensionTagNum == kTag_BasicConstraints ||
                extensionTagNum == kTag_ExtendedKeyUsage)
            {
                ASN1_ENCODE_BOOLEAN(true);
            }

            // extnValue OCTET STRING
            //           -- contains the DER encoding of an ASN.1 value
            //           -- corresponding to the extension type identified
            //           -- by extnID
            ASN1_START_OCTET_STRING_ENCAPSULATED
            {
                if (extensionTagNum == kTag_AuthorityKeyIdentifier)
                {
                    ReturnErrorOnFailure(DecodeConvertAuthorityKeyIdentifierExtension(reader, writer, certData));
                }
                else if (extensionTagNum == kTag_SubjectKeyIdentifier)
                {
                    ReturnErrorOnFailure(DecodeConvertSubjectKeyIdentifierExtension(reader, writer, certData));
                }
                else if (extensionTagNum == kTag_KeyUsage)
                {
                    ReturnErrorOnFailure(DecodeConvertKeyUsageExtension(reader, writer, certData));
                }
                else if (extensionTagNum == kTag_BasicConstraints)
                {
                    ReturnErrorOnFailure(DecodeConvertBasicConstraintsExtension(reader, writer, certData));
                }
                else if (extensionTagNum == kTag_ExtendedKeyUsage)
                {
                    ReturnErrorOnFailure(DecodeConvertExtendedKeyUsageExtension(reader, writer, certData));
                }
                else
                {
                    return CHIP_ERROR_UNSUPPORTED_CERT_FORMAT;
                }
            }
            ASN1_END_ENCAPSULATED;
        }
        ASN1_END_SEQUENCE;
    }

exit:
    return err;
}

static CHIP_ERROR DecodeConvertExtensions(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    TLVType outerContainer;

    ReturnErrorOnFailure(reader.Next(kTLVType_List, ContextTag(kTag_Extensions)));
    ReturnErrorOnFailure(reader.EnterContainer(outerContainer));

    // extensions [3] EXPLICIT Extensions OPTIONAL
    ASN1_START_CONSTRUCTED(kASN1TagClass_ContextSpecific, 3)
    {
        // Extensions ::= SEQUENCE SIZE (1..MAX) OF Extension
        ASN1_START_SEQUENCE
        {
            // Read certificate extension in the List.
            while ((err = reader.Next()) == CHIP_NO_ERROR)
            {
                ReturnErrorOnFailure(DecodeConvertExtension(reader, writer, certData));
            }
            VerifyOrReturnError(err == CHIP_END_OF_TLV, err);
        }
        ASN1_END_SEQUENCE;
    }
    ASN1_END_CONSTRUCTED;

    ReturnErrorOnFailure(reader.ExitContainer(outerContainer));

exit:
    return err;
}

static CHIP_ERROR DecodeECDSASignature(TLVReader & reader, ChipCertificateData & certData)
{
    ReturnErrorOnFailure(reader.Next(kTLVType_ByteString, ContextTag(kTag_ECDSASignature)));
    ReturnErrorOnFailure(reader.Get(certData.mSignature));
    return CHIP_NO_ERROR;
}

static CHIP_ERROR DecodeConvertECDSASignature(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    ReturnErrorOnFailure(DecodeECDSASignature(reader, certData));

    // Converting the signature is a bit of work, so explicitly check if we have a null writer
    VerifyOrReturnError(!writer.IsNullWriter(), CHIP_NO_ERROR);

    // signatureValue BIT STRING
    // Per RFC3279, the ECDSA signature value is encoded in DER encapsulated in the signatureValue BIT STRING.
    ASN1_START_BIT_STRING_ENCAPSULATED
    {
        ReturnErrorOnFailure(ConvertECDSASignatureRawToDER(certData.mSignature, writer));
    }
    ASN1_END_ENCAPSULATED;

exit:
    return err;
}

/**
 * @brief Decode and convert the To-Be-Signed (TBS) portion of the CHIP certificate
 *        into X.509 DER encoded form.
 *
 * @param reader    A TLVReader positioned at the beginning of the TBS portion
 *                  (certificate serial number) of the CHIP certificates.
 * @param writer    A reference to the ASN1Writer to store DER encoded TBS portion of
 *                  the CHIP certificate.
 * @param certData  Structure containing data extracted from the TBS portion of the
 *                  CHIP certificate.
 *
 * Note: The reader must be positioned on the SerialNumber element.
 *
 * @return Returns a CHIP_ERROR on error, CHIP_NO_ERROR otherwise
 **/
static CHIP_ERROR DecodeConvertTBSCert(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    CHIP_ERROR err;

    // tbsCertificate TBSCertificate,
    // TBSCertificate ::= SEQUENCE
    ASN1_START_SEQUENCE
    {
        // version [0] EXPLICIT Version DEFAULT v1
        ASN1_START_CONSTRUCTED(kASN1TagClass_ContextSpecific, 0)
        {
            // Version ::= INTEGER { v1(0), v2(1), v3(2) }
            ASN1_ENCODE_INTEGER(2);
        }
        ASN1_END_CONSTRUCTED;

        // serialNumber CertificateSerialNumber
        // CertificateSerialNumber ::= INTEGER
        ReturnErrorOnFailure(reader.Expect(kTLVType_ByteString, ContextTag(kTag_SerialNumber)));
        ReturnErrorOnFailure(reader.Get(certData.mSerialNumber));
        ReturnErrorOnFailure(writer.PutValue(kASN1TagClass_Universal, kASN1UniversalTag_Integer, false,
                                             certData.mSerialNumber.data(), static_cast<uint16_t>(certData.mSerialNumber.size())));

        // signature AlgorithmIdentifier
        // AlgorithmIdentifier ::= SEQUENCE
        ASN1_START_SEQUENCE
        {
            uint8_t sigAlgoId;
            ReturnErrorOnFailure(reader.Next(ContextTag(kTag_SignatureAlgorithm)));
            ReturnErrorOnFailure(reader.Get(sigAlgoId));

            certData.mSigAlgoOID = GetOID(kOIDCategory_SigAlgo, sigAlgoId);
            ASN1_ENCODE_OBJECT_ID(certData.mSigAlgoOID);
        }
        ASN1_END_SEQUENCE;

        // issuer Name
        ReturnErrorOnFailure(reader.Next(kTLVType_List, ContextTag(kTag_Issuer)));
        ReturnErrorOnFailure(DecodeConvertDN(reader, writer, certData.mIssuerDN));

        // validity Validity,
        ReturnErrorOnFailure(DecodeConvertValidity(reader, writer, certData));

        // subject Name
        ReturnErrorOnFailure(reader.Next(kTLVType_List, ContextTag(kTag_Subject)));
        ReturnErrorOnFailure(DecodeConvertDN(reader, writer, certData.mSubjectDN));

        // subjectPublicKeyInfo SubjectPublicKeyInfo,
        ReturnErrorOnFailure(DecodeConvertSubjectPublicKeyInfo(reader, writer, certData));

        // certificate extensions
        ReturnErrorOnFailure(DecodeConvertExtensions(reader, writer, certData));
    }
    ASN1_END_SEQUENCE;

exit:
    return err;
}

/**
 * Variant of DecodeConvertTBSCert that handles reading a compact-pdc-identity
 * where only the subject public key is actually encoded. All other values are
 * populated / written as the well-known values mandated by the specification.
 *
 * Note: The reader must be positioned on the EllipticCurvePublicKey element.
 */
static CHIP_ERROR DecodeConvertTBSCertCompactIdentity(TLVReader & reader, ASN1Writer & writer, ChipCertificateData & certData)
{
    // Decode the public key, everything else is rigid
    ReturnErrorOnFailure(reader.Expect(kTLVType_ByteString, ContextTag(kTag_EllipticCurvePublicKey)));
    ReturnErrorOnFailure(reader.Get(certData.mPublicKey));

    // Populate rigid ChipCertificateData fields
    certData.mSerialNumber = kNetworkIdentitySerialNumberBytes;
    certData.mSigAlgoOID   = kOID_SigAlgo_ECDSAWithSHA256;
    InitNetworkIdentitySubject(certData.mIssuerDN);
    certData.mNotBeforeTime = kNetworkIdentityNotBeforeTime;
    certData.mNotAfterTime  = kNetworkIdentityNotAfterTime;
    InitNetworkIdentitySubject(certData.mSubjectDN);
    certData.mPubKeyAlgoOID  = kOID_PubKeyAlgo_ECPublicKey;
    certData.mPubKeyCurveOID = kOID_EllipticCurve_prime256v1;
    certData.mCertFlags.Set(CertFlags::kExtPresent_BasicConstraints);
    certData.mCertFlags.Set(CertFlags::kExtPresent_KeyUsage);
    certData.mKeyUsageFlags = kNetworkIdentityKeyUsage;
    certData.mCertFlags.Set(CertFlags::kExtPresent_ExtendedKeyUsage);
    certData.mKeyPurposeFlags = kNetworkIdentityKeyPurpose;

    if (!writer.IsNullWriter())
    {
        ReturnErrorOnFailure(EncodeNetworkIdentityTBSCert(certData.mPublicKey, writer));
    }
    return CHIP_NO_ERROR;
}

/**
 * Decode a CHIP TLV certificate and convert it to X.509 DER form.
 *
 * This helper function takes separate ASN1Writers for the whole Certificate
 * and the TBSCertificate, to allow the caller to control which part (if any)
 * to capture.
 *
 * If `writer` is NOT a null writer, then `tbsWriter` MUST be a reference
 * to the same writer, otherwise the overall Certificate written will not be
 * valid.
 */
static CHIP_ERROR DecodeConvertCert(TLVReader & reader, ASN1Writer & writer, ASN1Writer & tbsWriter, ChipCertificateData & certData)
{
    CHIP_ERROR err;
    TLVType containerType;

    if (reader.GetType() == kTLVType_NotSpecified)
    {
        ReturnErrorOnFailure(reader.Next());
    }
    ReturnErrorOnFailure(reader.Expect(kTLVType_Structure, AnonymousTag()));
    ReturnErrorOnFailure(reader.EnterContainer(containerType));

    // Certificate ::= SEQUENCE
    ASN1_START_SEQUENCE
    {
        // tbsCertificate TBSCertificate,
        reader.Next();
        if (reader.GetTag() == ContextTag(kTag_EllipticCurvePublicKey))
        {
            // If the struct starts with the ec-pub-key we're dealing with a
            // Network (Client) Identity in compact-pdc-identity format.
            DecodeConvertTBSCertCompactIdentity(reader, tbsWriter, certData);
        }
        else
        {
            ReturnErrorOnFailure(DecodeConvertTBSCert(reader, tbsWriter, certData));
        }

        // signatureAlgorithm   AlgorithmIdentifier
        // AlgorithmIdentifier ::= SEQUENCE
        ASN1_START_SEQUENCE
        {
            ASN1_ENCODE_OBJECT_ID(static_cast<OID>(certData.mSigAlgoOID));
        }
        ASN1_END_SEQUENCE;

        // signatureValue BIT STRING
        ReturnErrorOnFailure(DecodeConvertECDSASignature(reader, writer, certData));
    }
    ASN1_END_SEQUENCE;

    // Verify no more elements in certificate.
    ReturnErrorOnFailure(reader.VerifyEndOfContainer());
    ReturnErrorOnFailure(reader.ExitContainer(containerType));

exit:
    return err;
}

DLL_EXPORT CHIP_ERROR ConvertChipCertToX509Cert(const ByteSpan chipCert, MutableByteSpan & x509Cert)
{
    TLVReader reader;
    ASN1Writer writer;
    ChipCertificateData certData;

    reader.Init(chipCert);

    writer.Init(x509Cert);

    certData.Clear();

    ReturnErrorOnFailure(DecodeConvertCert(reader, writer, writer, certData));

    x509Cert.reduce_size(writer.GetLengthWritten());

    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodeChipCert(const ByteSpan chipCert, ChipCertificateData & certData, BitFlags<CertDecodeFlags> decodeFlags)
{
    TLVReader reader;

    reader.Init(chipCert);
    return DecodeChipCert(reader, certData, decodeFlags);
}

CHIP_ERROR DecodeChipCert(TLVReader & reader, ChipCertificateData & certData, BitFlags<CertDecodeFlags> decodeFlags)
{
    ASN1Writer nullWriter;
    nullWriter.InitNullWriter();

    certData.Clear();

    if (decodeFlags.Has(CertDecodeFlags::kGenerateTBSHash))
    {
        // Create a buffer and writer to capture the TBS (to-be-signed) portion of the certificate
        // when we decode (and convert) the certificate, so we can hash it to create the TBSHash.
        chip::Platform::ScopedMemoryBuffer<uint8_t> asn1TBSBuf;
        VerifyOrReturnError(asn1TBSBuf.Alloc(kMaxCHIPCertDecodeBufLength), CHIP_ERROR_NO_MEMORY);
        ASN1Writer tbsWriter;
        tbsWriter.Init(asn1TBSBuf.Get(), kMaxCHIPCertDecodeBufLength);

        ReturnErrorOnFailure(DecodeConvertCert(reader, nullWriter, tbsWriter, certData));

        // Hash the encoded TBS certificate. Only SHA256 is supported.
        VerifyOrReturnError(certData.mSigAlgoOID == kOID_SigAlgo_ECDSAWithSHA256, CHIP_ERROR_UNSUPPORTED_SIGNATURE_TYPE);
        ReturnErrorOnFailure(Hash_SHA256(asn1TBSBuf.Get(), tbsWriter.GetLengthWritten(), certData.mTBSHash));
        certData.mCertFlags.Set(CertFlags::kTBSHashPresent);
    }
    else
    {
        ReturnErrorOnFailure(DecodeConvertCert(reader, nullWriter, nullWriter, certData));
    }

    // If requested by the caller, mark the certificate as trusted.
    if (decodeFlags.Has(CertDecodeFlags::kIsTrustAnchor))
    {
        certData.mCertFlags.Set(CertFlags::kIsTrustAnchor);
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodeChipDN(TLVReader & reader, ChipDN & dn)
{
    ASN1Writer writer;

    writer.InitNullWriter();

    dn.Clear();

    return DecodeConvertDN(reader, writer, dn);
}

} // namespace Credentials
} // namespace chip
