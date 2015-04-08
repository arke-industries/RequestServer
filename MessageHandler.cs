﻿using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;
using System.Text;

namespace ArkeIndustries.RequestServer {
	public enum MessageParameterDirection {
		Input,
		Output
	}

	[AttributeUsage(AttributeTargets.Class, AllowMultiple = true)]
	public sealed class MessageDefinitionAttribute : Attribute {
		public long Id { get; }
		public long ServerId { get; }
		public long AuthenticationLevelRequired { get; }

		public MessageDefinitionAttribute(long id, long serverId, long authenticationLevelRequired) {
			this.Id = id;
			this.ServerId = serverId;
			this.AuthenticationLevelRequired = authenticationLevelRequired;
		}
	}

	[AttributeUsage(AttributeTargets.Property)]
	public sealed class MessageParameterAttribute : Attribute {
		public long Index { get; }
		public MessageParameterDirection Direction { get; }

		public MessageParameterAttribute(long index, MessageParameterDirection direction) {
			this.Index = index;
			this.Direction = direction;
		}
	}

	public abstract class MessageHandler {
		internal class ParameterNode {
			public PropertyInfo Property { get; set; }
			public Type ListGenericType { get; set; }
			public ISerializationDefinition ListMemberSerializationDefinition { get; set; }
			public ISerializationDefinition SerializationDefinition { get; set; }
			public List<ParameterNode> Children { get; set; }
		}

		internal class BoundProperty {
			public PropertyInfo Property { get; set; }
			public ParameterNode Parameter { get; set; }
		}

		private class ValidationProperty {
			public List<ValidationAttribute> Attributes { get; set; }
			public PropertyInfo Property { get; set; }
		}

		internal interface ISerializationDefinition {
			void Serialize(BinaryWriter writer, ParameterNode node, object obj);
			object Deserialize(BinaryReader reader, ParameterNode node);
		}

		internal class SerializationDefinition<T> : ISerializationDefinition {
			public Action<BinaryWriter, ParameterNode, T> Serializer { get; set; }
			public Func<BinaryReader, ParameterNode, T> Deserializer { get; set; }

			public void Serialize(BinaryWriter writer, ParameterNode node, object obj) => this.Serializer(writer, node, (T)obj);
			public object Deserialize(BinaryReader reader, ParameterNode node) => this.Deserializer(reader, node);
		}

		public static DateTime DateTimeEpoch { get; set; } = new DateTime(2015, 1, 1, 0, 0, 0);

		private List<ParameterNode> inputProperties;
		private List<ParameterNode> outputProperties;
		private List<BoundProperty> boundProperties;
		private List<ValidationProperty> validationProperties;
		private Dictionary<Type, ISerializationDefinition> serializationDefinitions;

		public MessageContext Context { get; set; }

		internal List<Notification> GeneratedNotifications { get; private set; }

		public virtual long Perform() => ResponseCode.Success;

		protected void BindObjectToResponse(object source) => this.BindObjectToResponse(source, MessageParameterDirection.Output);
		protected void SendNotification(long targetAuthenticatedId, long notificationType) => this.SendNotification(targetAuthenticatedId, notificationType, 0);
		protected void SendNotification(long targetAuthenticatedId, long notificationType, long objectId) => this.GeneratedNotifications.Add(new Notification(targetAuthenticatedId, notificationType, objectId));

		private void AddSerializationDefinition<T>(Action<BinaryWriter, ParameterNode, T> serializer, Func<BinaryReader, ParameterNode, T> deserializer) => this.serializationDefinitions.Add(typeof(T), new SerializationDefinition<T> { Serializer = serializer, Deserializer = deserializer });

		protected MessageHandler() {
			this.GeneratedNotifications = new List<Notification>();

			this.AddSerializationDefinitions();

			this.inputProperties = this.CreateTree(MessageParameterDirection.Input);
			this.outputProperties = this.CreateTree(MessageParameterDirection.Output);

			this.validationProperties = this.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)
				.Where(p => p.IsDefined(typeof(ValidationAttribute)))
				.Select(p => new ValidationProperty() { Property = p, Attributes = p.GetCustomAttributes<ValidationAttribute>().ToList() })
				.ToList();
		}

		[SuppressMessage("Microsoft.Maintainability", "CA1502")]
		private void AddSerializationDefinitions() {
			this.serializationDefinitions = new Dictionary<Type, ISerializationDefinition>();

			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadString());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadBoolean());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadByte());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadSByte());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadUInt16());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadInt16());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadUInt32());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadInt32());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadUInt64());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadInt64());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadSingle());
			this.AddSerializationDefinition((w, p, o) => w.Write(o), (r, p) => r.ReadDouble());
			this.AddSerializationDefinition((w, p, o) => this.SerializeList(w, p, o), (r, p) => this.DeserializeList(r, p));
			this.AddSerializationDefinition((w, p, o) => w.Write((ulong)((o - MessageHandler.DateTimeEpoch).TotalMilliseconds)), (r, p) => MessageHandler.DateTimeEpoch.AddMilliseconds(r.ReadUInt64()));
		}

		public long IsValid() {
			foreach (var p in this.validationProperties) {
				var value = p.Property.GetValue(this);

				foreach (var v in p.Attributes) {
					var valid = v.IsValid(value, this.Context);

					if (valid != ResponseCode.Success)
						return valid;
				}
			}

			return ResponseCode.Success;
		}

		public void Serialize(MessageParameterDirection direction, Stream stream) {
			if (stream == null) throw new ArgumentNullException(nameof(stream));

			using (var writer = new BinaryWriter(stream, Encoding.UTF8, true))
				foreach (var property in direction == MessageParameterDirection.Input ? this.inputProperties : this.outputProperties)
					this.Serialize(writer, property, this);
		}

		public void Deserialize(MessageParameterDirection direction, Stream stream) {
			if (stream == null) throw new ArgumentNullException(nameof(stream));

			using (var reader = new BinaryReader(stream, Encoding.UTF8, true))
				foreach (var property in direction == MessageParameterDirection.Input ? this.inputProperties : this.outputProperties)
					this.Deserialize(reader, property, this);
		}

		protected void BindObjectToResponse(object source, MessageParameterDirection direction) {
			if (source == null) throw new ArgumentNullException(nameof(source));

			if (this.boundProperties == null)
				this.boundProperties = MessageHandler.GetPropertiesToBind(source.GetType(), direction == MessageParameterDirection.Input ? this.inputProperties : this.outputProperties);

			foreach (var p in this.boundProperties)
				p.Parameter.Property.SetValue(this, Convert.ChangeType(p.Property.GetValue(source), p.Property.PropertyType, CultureInfo.InvariantCulture));
		}

		internal static List<BoundProperty> GetPropertiesToBind(Type type, List<ParameterNode> targetProperties) {
			var sourceProperties = type.GetProperties();

			return targetProperties
				.Where(p => sourceProperties.Any(s => s.Name == p.Property.Name))
				.Select(p => new BoundProperty() { Parameter = p, Property = sourceProperties.SingleOrDefault(b => b.Name == p.Property.Name) })
				.ToList();
		}

		internal static List<PropertyInfo> GetProperties(Type type, MessageParameterDirection direction) {
			return type.GetProperties(BindingFlags.Public | BindingFlags.Instance)
				.Where(p => p.IsDefined(typeof(MessageParameterAttribute)))
				.Where(p => p.GetCustomAttribute<MessageParameterAttribute>().Direction == direction)
				.OrderBy(p => p.GetCustomAttribute<MessageParameterAttribute>().Index)
				.ToList();
		}

		private List<ParameterNode> CreateTree(MessageParameterDirection direction) {
			return this.CreateTree(direction, this.GetType());
		}

		internal List<ParameterNode> CreateTree(MessageParameterDirection direction, Type type) {
			return MessageHandler.GetProperties(type, direction).Select(p => this.CreateTree(direction, p)).ToList();
		}

		private ParameterNode CreateTree(MessageParameterDirection direction, PropertyInfo property) {
			var node = new ParameterNode() { Property = property };
			var iface = property.PropertyType.GetInterfaces().SingleOrDefault(i => i.IsGenericType && i.GetGenericTypeDefinition() == typeof(IList<>));

			if (iface != null) {
				node.ListGenericType = iface.GenericTypeArguments.Single();
				node.ListMemberSerializationDefinition = this.serializationDefinitions[node.ListGenericType];
				node.SerializationDefinition = this.serializationDefinitions[typeof(IList)];
				node.Children = MessageHandler.GetProperties(node.ListGenericType, direction).Select(p => this.CreateTree(direction, p)).ToList();
			}
			else if (property.PropertyType.IsEnum) {
				node.SerializationDefinition = this.serializationDefinitions[Enum.GetUnderlyingType(node.Property.PropertyType)];
			}
			else {
				node.SerializationDefinition = this.serializationDefinitions[node.Property.PropertyType];
				node.Children = MessageHandler.GetProperties(property.PropertyType, direction).Select(p => this.CreateTree(direction, p)).ToList();
			}

			return node;
		}

		private void Serialize(BinaryWriter writer, ParameterNode node, object obj) {
			var value = node.Property.GetValue(obj);

			if (node.SerializationDefinition != null) {
				node.SerializationDefinition.Serialize(writer, node, value);
			}
			else {
				node.Children.ForEach(f => this.Serialize(writer, f, value));
			}
		}

		private void Deserialize(BinaryReader reader, ParameterNode node, object obj) {
			if (node.SerializationDefinition != null) {
				node.Property.SetValue(obj, node.SerializationDefinition.Deserialize(reader, node));
			}
			else {
				node.Children.ForEach(f => this.Deserialize(reader, f, node.Property.GetValue(obj)));
			}
		}

		private void SerializeList(BinaryWriter writer, ParameterNode node, IList collection) {
			writer.Write((ushort)collection.Count);

			if (collection.Count == 0)
				return;

			for (var i = 0; i < collection.Count; i++) {
				if (node.Children.Any()) {
					node.Children.ForEach(f => this.Serialize(writer, f, collection[i]));
				}
				else {
					node.ListMemberSerializationDefinition.Serialize(writer, node, collection[i]);
                }
			}

			collection.Clear();
		}

		private IList DeserializeList(BinaryReader reader, ParameterNode node) {
			var count = reader.ReadUInt16();
			var collectionConstructor = node.Property.PropertyType.GetConstructor(!node.Property.PropertyType.IsArray ? Type.EmptyTypes : new Type[] { typeof(int) });
			var collection = (IList)collectionConstructor.Invoke(!node.Property.PropertyType.IsArray ? null : new object[] { count });
			var adder = !node.Property.PropertyType.IsArray ? (Action<object, int>)((o, i) => collection.Add(o)) : (o, i) => collection[i] = o;

			if (node.Children.Any()) {
				var objectConstructor = node.ListGenericType.GetConstructor(Type.EmptyTypes);

				for (var i = 0; i < count; i++) {
					var newObject = objectConstructor.Invoke(null);

					node.Children.ForEach(f => this.Deserialize(reader, f, newObject));

					adder(newObject, i);
				}
			}
			else {
				for (var i = 0; i < count; i++) {
					adder(node.ListMemberSerializationDefinition.Deserialize(reader, node), i);
				}
			}

			return collection;
		}
	}

	public abstract class MessageHandler<T> : MessageHandler where T : MessageContext {
	public new T Context {
		get {
			return (T)base.Context;
		}
		set {
			base.Context = value;
		}
	}
}

public abstract class ListMessageHandler<TContext, TEntry> : MessageHandler<TContext> where TContext : MessageContext where TEntry : new() {
	private List<BoundProperty> boundProperties;

	[MessageParameter(-4, MessageParameterDirection.Input)]
	[AtLeast(0)]
	public int Skip { get; set; }

	[MessageParameter(-3, MessageParameterDirection.Input)]
	[AtLeast(0)]
	public int Take { get; set; }

	[MessageParameter(-2, MessageParameterDirection.Input)]
	[ApiString(false, 1)]
	public string OrderByField { get; set; }

	[MessageParameter(-1, MessageParameterDirection.Input)]
	public bool OrderByAscending { get; set; }

	[MessageParameter(-1, MessageParameterDirection.Output)]
	public IReadOnlyList<TEntry> List { get; private set; }

	protected void SetResponse(IQueryable<TEntry> query) {
		if (query == null) throw new ArgumentNullException(nameof(query));

		var parameter = Expression.Parameter(typeof(TEntry));
		var property = Expression.Property(parameter, this.OrderByField);
		var sort = Expression.Lambda(property, parameter);
		var quote = Expression.Quote(sort);
		var call = Expression.Call(typeof(Queryable), this.OrderByAscending ? "OrderBy" : "OrderByDescending", new[] { typeof(TEntry), property.Type }, query.Expression, quote);

		this.List = query.Provider.CreateQuery<TEntry>(call).Skip(this.Skip).Take(this.Take).ToList();
	}

	protected void BindListToResponse<T>(IQueryable<T> query) {
		if (query == null) throw new ArgumentNullException(nameof(query));

		var parameter = Expression.Parameter(typeof(T));
		var property = Expression.Property(parameter, this.OrderByField);
		var sort = Expression.Lambda(property, parameter);
		var quote = Expression.Quote(sort);
		var call = Expression.Call(typeof(Queryable), this.OrderByAscending ? "OrderBy" : "OrderByDescending", new[] { typeof(T), property.Type }, query.Expression, quote);

		if (this.boundProperties == null)
			this.boundProperties = MessageHandler.GetPropertiesToBind(typeof(T), this.CreateTree(MessageParameterDirection.Output, typeof(TEntry)));

		var result = new List<TEntry>();
		foreach (var sourceEntry in query.Provider.CreateQuery<T>(call).Skip(this.Skip).Take(this.Take)) {
			var resultEntry = new TEntry();

			foreach (var p in this.boundProperties)
				p.Parameter.Property.SetValue(resultEntry, Convert.ChangeType(p.Property.GetValue(sourceEntry), p.Property.PropertyType, CultureInfo.InvariantCulture));

			result.Add(resultEntry);
		}

		this.List = result;
	}
}
}