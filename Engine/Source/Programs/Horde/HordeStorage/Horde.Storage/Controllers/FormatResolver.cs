// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Linq;
using System.Net.Mime;
using Jupiter;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Options;
using Microsoft.Extensions.Primitives;

namespace Horde.Storage.Controllers
{
    public class FormatResolver
    {
        private readonly IOptionsMonitor<MvcOptions> _mvcOptions;

        private readonly string[] _validContentTypes = {
            MediaTypeNames.Application.Octet, MediaTypeNames.Application.Json, CustomMediaTypeNames.UnrealCompactBinary
        };

        public FormatResolver(IOptionsMonitor<MvcOptions> mvcOptions)
        {
            _mvcOptions = mvcOptions;
        }

        public string GetResponseType(HttpRequest request, string? format, string defaultContentType)
        {
            // if format specifier is used it takes precedence over the accept header
            if (format != null)
            {
                string? typeMapping = _mvcOptions.CurrentValue.FormatterMappings.GetMediaTypeMappingForFormat(format);
                if (typeMapping == null)
                    throw new Exception($"No mapping defined from format {format} to mime type");
                return typeMapping;
            }
            
            StringValues acceptHeader = request.Headers["Accept"];

            if (acceptHeader.Count == 0)
            {
                // no accept header specified, return default type
                return defaultContentType;
            }

            foreach (string header in acceptHeader)
            {
                string s = header.ToLowerInvariant();
                if (_validContentTypes.Contains(s))
                {
                    return s;
                }
            }

            throw new Exception($"Unable to determine response type for header: {acceptHeader}");
        }
    }
}
