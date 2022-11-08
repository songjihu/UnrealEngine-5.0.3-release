// Copyright Epic Games, Inc. All Rights Reserved.

// This file is used by Code Analysis to maintain SuppressMessage
// attributes that are applied to this project.
// Project-level suppressions either have no target or are given
// a specific target and scoped to a namespace, type, member, etc.

using System.Diagnostics.CodeAnalysis;

[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Reliability", "CA2007:Consider calling ConfigureAwait on the awaited task", Justification = "Not necessary in NET Core; no SynchronizationContext.", Scope = "module")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Usage", "CA2227:Collection properties should be read only", Justification = "<Pending>", Scope = "module")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Design", "CA1062:Validate arguments of public methods", Justification = "<Pending>", Scope = "module")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Globalization", "CA1303:Do not pass literals as localized parameters", Justification = "<Pending>", Scope = "module")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Performance", "CA1819:Properties should not return arrays", Justification = "<Pending>", Scope = "module")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1716:Identifiers should not match keywords", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Models.Event")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Design", "CA1032:Implement standard exception constructors", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Services.RetryNotAllowedException")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1714:Flags enums should have plural names", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Api.ScheduleDaysOfWeek")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1720:Identifier contains type name", Justification = "<Pending>", Scope = "member", Target = "~F:HordeServer.Api.TemplateParameterType.String")]

[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1822:Member Login does not access instance data and can be marked as static (Shared in VisualBasic)", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Controllers.AccountController.Login~Microsoft.AspNetCore.Mvc.IActionResult")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1031:Modify 'Logout' to catch a more specific allowed exception type, or rethrow the exception.", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Controllers.AccountController.Logout~System.Threading.Tasks.Task{Microsoft.AspNetCore.Mvc.IActionResult}")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1305:The behavior of 'string.ToString()' could vary based on the current user's locale settings. Replace this call in 'AgentLease.ToResponse()' with a call to 'string.ToString(IFormatProvider)'.", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Models.AgentLease.ToResponse~HordeServer.Api.GetAgentLeaseResponse")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA5380:Adding certificates to the operating system's trusted root certificates increases the risk of incorrectly authenticating an illegitimate certificate", Justification = "<Pending>", Scope = "member", Target = "~T:HordeServer.Services.DatabaseService")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1724:The type name Template conflicts in whole or in part with the namespace name 'Microsoft.AspNetCore.Routing.Template'. Change either name to eliminate the conflict.", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Models.Template")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1724:The type name Session conflicts in whole or in part with the namespace name 'Microsoft.AspNetCore.Session'. Change either name to eliminate the conflict.", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Models.Session")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1812:BotsService is an internal class that is apparently never instantiated. If so, remove the code from the assembly. If this class is intended to contain only static members, make it static (Shared in Visual Basic).", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Services.BotsService")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1812:RpcService is an internal class that is apparently never instantiated. If so, remove the code from the assembly. If this class is intended to contain only static members, make it static (Shared in Visual Basic).", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Services.RpcService")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1812:Startup is an internal class that is apparently never instantiated. If so, remove the code from the assembly. If this class is intended to contain only static members, make it static (Shared in Visual Basic).", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Startup")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1812:Startup.AgentJwtBearerHandler is an internal class that is apparently never instantiated. If so, remove the code from the assembly. If this class is intended to contain only static members, make it static (Shared in Visual Basic).", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Startup.AgentJwtBearerHandler")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Build", "CA1812:Startup.AnonymousAuthenticationHandler is an internal class that is apparently never instantiated. If so, remove the code from the assembly. If this class is intended to contain only static members, make it static (Shared in Visual Basic).", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Startup.AnonymousAuthenticationHandler")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Performance", "CA1822:Mark members as static", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Controllers.DebugController.Environment~Microsoft.AspNetCore.Mvc.IActionResult")]
[assembly: System.Diagnostics.CodeAnalysis.SuppressMessage("Naming", "CA1716:Identifiers should not match keywords", Justification = "<Pending>", Scope = "module")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Services.AgentService.ExecuteAsync(System.Threading.CancellationToken)~System.Threading.Tasks.Task")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Utilities.PeriodicBackgroundService.ExecuteAsync(System.Threading.CancellationToken)~System.Threading.Tasks.Task")]
[assembly: SuppressMessage("Performance", "CA1815:Override equals and operator equals on value types", Justification = "<Pending>", Scope = "type", Target = "~T:HordeServer.Models.JobStepRefId")]
[assembly: SuppressMessage("Style", "IDE0054:Use compound assignment", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Models.Job.GetTimingInfo~System.Collections.Generic.Dictionary{HordeServer.Models.Node,HordeServer.Models.TimingInfo}")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Startup.DashboardBearerHandler.HandleAuthenticateAsync~System.Threading.Tasks.Task{Microsoft.AspNetCore.Authentication.AuthenticateResult}")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Services.ConformService.ExecuteAsync(System.Threading.CancellationToken)~System.Threading.Tasks.Task")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Services.ScheduleService.ExecuteAsync(System.Threading.CancellationToken)~System.Threading.Tasks.Task")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "member", Target = "~M:HordeServer.Services.ScheduleService.UpdateSchedulesAsync~System.Threading.Tasks.Task{System.DateTimeOffset}")]
[assembly: SuppressMessage("Design", "CA1030:Use events where appropriate", Justification = "<Pending>", Scope = "module")]
[assembly: SuppressMessage("Design", "CA1031:Do not catch general exception types", Justification = "<Pending>", Scope = "module")]
[assembly: SuppressMessage("Design", "CA1032:Implement standard exception constructors", Justification = "<Pending>", Scope = "module")]
